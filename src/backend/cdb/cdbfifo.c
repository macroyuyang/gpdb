#include "postgres.h"

#include <sys/types.h>
#include <sys/stat.h>
#include <poll.h>
#include <unistd.h>

#include "nodes/value.h"
#include "storage/ipc.h"
#include "storage/procsignal.h"
#include "storage/s_lock.h"
#include "utils/elog.h"
#include "cdb/cdbfifo.h"
#include "cdb/cdbvars.h"
#include "utils/gp_alloc.h"

#define MAX_ENDPOINT_SIZE 	100
#define MAX_FIFO_NAME_SIZE	100
#define InvalidToken		(-1)
#define InvalidPid			0
#define FIFO_NAME_PATTERN "/tmp/gp2gp_fifo_%d_%d"
#define SHMEM_END_POINT "SharedMemoryEndPoint"
#define SHMEM_END_POINT_SLOCK "SharedMemoryEndPointSlock"

#define ep_log(level, ...) \
	do { \
		if (!s_inAbort)	\
			elog(level, __VA_ARGS__); \
	} \
	while (0)

const long POLL_FIFO_TIMEOUT = 500;

typedef struct fifoconnstate
{	
	int32       	fifo;
	bool			finished;
	bool			created;
} FifoConnStateData;

typedef FifoConnStateData 	*FifoConnState;

static FifoConnState 		s_fifoConnState = NULL;
static TupleTableSlot		*s_resultTupleSlot;

static EndPointDesc 		*SharedEndPoints;
volatile EndPointDesc 		*mySharedEndPoint = NULL;

static slock_t 				*shared_end_points_lock;

static int32 				Gp_token = InvalidToken;
static enum EndPointRole 	Gp_endpoint_role = EPR_NONE;
static bool					s_inAbort = false;
static bool					s_needAck = false;

void SetGpToken(int32 token)
{
	if (Gp_token != InvalidToken)
		ep_log(ERROR, "end point token %d already set", Gp_token);

	Gp_token = token;
	ep_log(LOG, "end point token %d set", Gp_token);
}

void ClearGpToken(void)
{
	ep_log(LOG, "end point token %d unset", Gp_token);

	Gp_token = InvalidToken;
}

int32 GpToken(void)
{
	return Gp_token;
}

static const char*
endpoint_role_to_string(enum EndPointRole role)
{
	switch (role)
	{
	case EPR_SENDER:
		return "[END POINT SENDER]";

	case EPR_RECEIVER:
		return "[END POINT RECEIVER]";

	case EPR_NONE:
		return "[END POINT NONE]";

	default:
		 ep_log(ERROR, "unknown end point role %d", role);
	}

	Assert(0);

	return NULL;
}

void SetEndPointRole(enum EndPointRole role)
{
	if (Gp_endpoint_role != EPR_NONE)
		ep_log(ERROR, "gp endpoint role %s already set",
			 endpoint_role_to_string(Gp_endpoint_role));

	ep_log(LOG, "set end point role to %s", endpoint_role_to_string(role));

	Gp_endpoint_role = role;
}

void ClearEndPointRole(void)
{
	ep_log(LOG, "unset end point role %s", endpoint_role_to_string(Gp_endpoint_role));

	Gp_endpoint_role = EPR_NONE;
}

enum EndPointRole EndPointRole(void)
{
	return Gp_endpoint_role;
}

static void check_gp_token_valid()
{
	if (Gp_token == InvalidToken)
		ep_log(ERROR, "invalid gp token");
}

Size EndPoint_ShmemSize()
{
	Size	size;

	size = mul_size(MAX_ENDPOINT_SIZE, sizeof(EndPointDesc));
	size = add_size(size, sizeof(slock_t));

	return size;
}

void EndPoint_ShmemInit()
{
	bool	is_shmem_ready;
	Size	size;

	size = mul_size(MAX_ENDPOINT_SIZE, sizeof(EndPointDesc));

    SharedEndPoints = (EndPointDesc *)
						ShmemInitStruct(SHMEM_END_POINT,
							size,
							&is_shmem_ready);

	Assert(is_shmem_ready || !IsUnderPostmaster);

	if (!is_shmem_ready)
	{
		int		i;

		for (i = 0; i < MAX_ENDPOINT_SIZE; ++i)
		{
			SharedEndPoints[i].database_id = InvalidOid;
			SharedEndPoints[i].sender_pid = InvalidPid;
			SharedEndPoints[i].receiver_pid = InvalidPid;
			SharedEndPoints[i].token = 0;
			SharedEndPoints[i].attached = false;
			SharedEndPoints[i].empty = true;
			SharedEndPoints[i].num_attributes = 0;

			InitSharedLatch(&SharedEndPoints[i].ack_done);
		}
	}

    shared_end_points_lock = (slock_t *)
						ShmemInitStruct(SHMEM_END_POINT_SLOCK,
							sizeof(slock_t),
							&is_shmem_ready);

	Assert(is_shmem_ready || !IsUnderPostmaster);

	if (!is_shmem_ready)
		SpinLockInit(shared_end_points_lock);
}

void AllocEndPoint(TupleDesc tupdesc)
{
	int			i;
	AttrDesc	attdesc[ENDPOINT_MAX_ATT_NUM];

	check_gp_token_valid();

	if (Gp_endpoint_role != EPR_SENDER)
		ep_log(ERROR, "%s could not allocate end point slot",
			   endpoint_role_to_string(Gp_endpoint_role));

	if (mySharedEndPoint)
		ep_log(ERROR, "end point slot already allocated");

	check_gp_token_valid();

	for (i = 0; i < tupdesc->natts; ++i)
	{
		Form_pg_attribute	attr = tupdesc->attrs[i];
		memcpy(attdesc[i].attname.data, attr->attname.data, NAMEDATALEN);
		attdesc[i].atttypid = attr->atttypid;
	}

	SpinLockAcquire(shared_end_points_lock);

	for (i = 0; i < MAX_ENDPOINT_SIZE; ++i)
	{
		if (SharedEndPoints[i].empty)
		{
			SharedEndPoints[i].database_id = MyDatabaseId;
			SharedEndPoints[i].sender_pid = MyProcPid;
			SharedEndPoints[i].token = Gp_token;
			SharedEndPoints[i].attached = false;
			SharedEndPoints[i].empty = false;
			SharedEndPoints[i].num_attributes = tupdesc->natts;
			memcpy(&SharedEndPoints[i].attdesc, attdesc, tupdesc->natts * sizeof(AttrDesc));
			OwnLatch(&SharedEndPoints[i].ack_done);

			mySharedEndPoint = &SharedEndPoints[i];
			break;
		}
	}

	SpinLockRelease(shared_end_points_lock);

	if (!mySharedEndPoint)
		ep_log(ERROR, "failed to allocate end point slot");
}

void FreeEndPoint()
{
	pid_t		receiver_pid;

	if (Gp_endpoint_role != EPR_SENDER)
		ep_log(ERROR, "%s can free end point slot", endpoint_role_to_string(Gp_endpoint_role));

	if (!mySharedEndPoint)
		ep_log(ERROR, "non end point slot allocated");
	
	check_gp_token_valid();

	while (true)
	{
		receiver_pid = InvalidPid;

		SpinLockAcquire(shared_end_points_lock);

		receiver_pid = mySharedEndPoint->receiver_pid;

		if (receiver_pid == InvalidPid)
		{
			mySharedEndPoint->database_id = InvalidOid;
			mySharedEndPoint->sender_pid = 0;
			mySharedEndPoint->token = InvalidToken;
			mySharedEndPoint->attached = false;
			mySharedEndPoint->empty = true;

			DisownLatch(&mySharedEndPoint->ack_done);
		}

		SpinLockRelease(shared_end_points_lock);

		if (receiver_pid != InvalidPid)
		{
			/*
			 * TODO: Kill receiver process and wait again
			 * to check if any other receiver to join.
			 */
			if (kill(receiver_pid, SIGINT) < 0)
				elog(WARNING, "failed to kill receiver process(pid:%d): %m", (int) receiver_pid);
		}
		else
			break;
	}

	mySharedEndPoint = NULL;
}

void AttachEndPoint()
{
	int			i;
	Size		num_attributes = 0;

	List		*names = NIL;
	List		*types = NIL;
	List		*typmods = NIL;
	List		*collations = NIL;
	bool		already_attached = false;
	pid_t		attached_pid = InvalidPid;

	TupleDesc	tupdesc;
	AttrDesc	attdesc[ENDPOINT_MAX_ATT_NUM];

	if (Gp_endpoint_role != EPR_RECEIVER)
		ep_log(ERROR, "%s could not attach end point slot", endpoint_role_to_string(Gp_endpoint_role));

	if (mySharedEndPoint)
		ep_log(ERROR, "end point slot already attached");
	
	check_gp_token_valid();

	SpinLockAcquire(shared_end_points_lock);

	for (i = 0; i < MAX_ENDPOINT_SIZE; ++i)
	{
		if (SharedEndPoints[i].database_id == MyDatabaseId &&
			SharedEndPoints[i].token == Gp_token &&
			!SharedEndPoints[i].empty)
		{
			if (SharedEndPoints[i].attached)
			{
				already_attached = true;
				attached_pid = SharedEndPoints[i].receiver_pid;
				break;
			}

			SharedEndPoints[i].attached = true;
			SharedEndPoints[i].receiver_pid = MyProcPid;
			mySharedEndPoint = &SharedEndPoints[i];
			num_attributes = SharedEndPoints[i].num_attributes;
			break;
		}
	}

	SpinLockRelease(shared_end_points_lock);

	if (already_attached)
		ep_log(ERROR, "end point %d already attached by receiver(pid:%d)",
			   Gp_token, attached_pid);

	if (!mySharedEndPoint)
		ep_log(ERROR, "failed to attach non exist end point %d", Gp_token);

	Assert(num_attributes < ENDPOINT_MAX_ATT_NUM);

	memcpy(attdesc, &mySharedEndPoint->attdesc , num_attributes * sizeof(AttrDesc));

	for (i = 0; i < num_attributes; ++i)
	{
		names = lappend(names, makeString(attdesc[i].attname.data));
		types = lappend_oid(types, attdesc[i].atttypid);
		typmods = lappend_int(typmods, -1);
		collations = lappend_oid(collations, InvalidOid);
	}

	tupdesc = BuildDescFromLists(names, types, typmods, collations);
	s_resultTupleSlot = MakeTupleTableSlot();
	ExecSetSlotDescriptor(s_resultTupleSlot, tupdesc);

	list_free_deep(names);
	list_free(types);
	list_free(typmods);
	list_free(collations);

	s_needAck = false;
}

void DetachEndPoint()
{
	volatile Latch	*ack_done;

	if (Gp_endpoint_role != EPR_RECEIVER ||
		!mySharedEndPoint ||
		Gp_token == InvalidToken)
		return;

	if (Gp_endpoint_role != EPR_RECEIVER)
		ep_log(ERROR, "%s could not attach end point slot", endpoint_role_to_string(Gp_endpoint_role));

	check_gp_token_valid();

	SpinLockAcquire(shared_end_points_lock);

	PG_TRY();
	{
		if (mySharedEndPoint->token != Gp_token)
			ep_log(LOG, "unmatched token, %d expected but %d met in slot",
				   Gp_token, mySharedEndPoint->token);

		if (mySharedEndPoint->receiver_pid != MyProcPid)
			ep_log(ERROR, "unmatched pid, %d expected but %d met in slot",
				 MyProcPid, mySharedEndPoint->receiver_pid);
	}
	PG_CATCH();
	{
		SpinLockRelease(shared_end_points_lock);
		PG_RE_THROW();
	}
	PG_END_TRY();

	mySharedEndPoint->receiver_pid = InvalidPid;
	mySharedEndPoint->attached = false;
	ack_done = &mySharedEndPoint->ack_done;

	SpinLockRelease(shared_end_points_lock);

	mySharedEndPoint = NULL;

	if (s_resultTupleSlot)
	{
		ExecDropSingleTupleTableSlot(s_resultTupleSlot);
		s_resultTupleSlot = NULL;
	}

	if (s_needAck)
		SetLatch(ack_done);

	s_needAck = false;
}

TupleDesc ResultTupleDesc()
{
	Assert(s_resultTupleSlot);
	Assert(s_resultTupleSlot->tts_tupleDescriptor);

	return s_resultTupleSlot->tts_tupleDescriptor;
}

static void make_fifo_conn(void)
{
	s_fifoConnState = (FifoConnStateData*) gp_malloc(sizeof(FifoConnStateData));

	Assert(s_fifoConnState);

	s_fifoConnState->fifo = -1;
	s_fifoConnState->created =false;
	s_fifoConnState->finished =false;
}

static void
check_end_point_allocated()
{
	if (Gp_endpoint_role != EPR_SENDER)
		ep_log(ERROR, "%s could not check end point slot allocated",
			   endpoint_role_to_string(Gp_endpoint_role));

	if (!mySharedEndPoint)
		ep_log(ERROR, "end point slot for token %d not allocated", Gp_token);

	check_gp_token_valid();

	SpinLockAcquire(shared_end_points_lock);

	if (mySharedEndPoint->token != Gp_token)
	{
		SpinLockRelease(shared_end_points_lock);
		ep_log(ERROR, "end point slot for token %d not allocated", Gp_token);
	}

	SpinLockRelease(shared_end_points_lock);
}

static void
create_and_connect_fifo()
{
	char	fifo_name[MAX_FIFO_NAME_SIZE];
	int 	flags;

	check_gp_token_valid();

	if (s_fifoConnState->created)
		return;

	snprintf(fifo_name, sizeof(fifo_name), FIFO_NAME_PATTERN, GpIdentity.segindex, Gp_token);

	if (mkfifo(fifo_name, 0666) < 0)
		ep_log(ERROR, "create fifo %s failed:%m", fifo_name);
	else
		s_fifoConnState->created = true;

	if (s_fifoConnState->fifo > 0)
		return;

	if ((s_fifoConnState->fifo = open(fifo_name, O_RDWR, 0666)) < 0)
	{
		CloseConn();
		ep_log(ERROR, "open fifo %s for write failed:%m", fifo_name);
	}

	flags = fcntl(s_fifoConnState->fifo, F_GETFL);

	if (flags < 0 || fcntl(s_fifoConnState->fifo, F_SETFL, flags | O_NONBLOCK) < 0)
	{
		CloseConn();
		ep_log(ERROR, "set nonblock fifo %s failed:%m", fifo_name);
	}
}

static void init_conn_for_sender()
{
	check_end_point_allocated();
	make_fifo_conn();
	create_and_connect_fifo();
}

static void init_conn_for_receiver()
{
	char	fifo_name[MAX_FIFO_NAME_SIZE];
	int 	flags;

	check_gp_token_valid();

	make_fifo_conn();

	snprintf(fifo_name, sizeof(fifo_name), FIFO_NAME_PATTERN, GpIdentity.segindex, Gp_token);

	if (s_fifoConnState->fifo > 0)
		return;

	if ((s_fifoConnState->fifo = open(fifo_name, O_RDWR, 0666)) < 0)
	{
		CloseConn();
		ep_log(ERROR, "failed to open fifo %s for read:%m", fifo_name);
	}

	flags = fcntl(s_fifoConnState->fifo, F_GETFL);

	if (flags < 0 || fcntl(s_fifoConnState->fifo, F_SETFL, flags | O_NONBLOCK) < 0)
	{
		CloseConn();
		ep_log(ERROR, "set nonblock fifo %s failed:%m", fifo_name);
	}
}

void InitConn()
{
	switch (Gp_endpoint_role)
	{
	case EPR_SENDER:
		init_conn_for_sender();
		break;
	case EPR_RECEIVER:
		init_conn_for_receiver();
		break;
	default:
		ep_log(ERROR, "none end point roles");
	}
}

static void
retry_write(int fifo, char *data, int len)
{
	int		wr;
	int		curr = 0;

	while (len > 0)
	{
		int		wrtRet;

		CHECK_FOR_INTERRUPTS();
		ResetLatch(&mySharedEndPoint->ack_done);

		wrtRet = write(fifo, &data[curr], len);
		if (wrtRet > 0)
		{
			curr += wrtRet;
			len -= wrtRet;
			continue;
		}
		else if (wrtRet == 0 && errno == EINTR)
			continue;
		else
		{
			if (errno != EAGAIN && errno != EWOULDBLOCK)
				ep_log(ERROR, "could not write to fifo:%m");
		}

		wr = WaitLatchOrSocket(&mySharedEndPoint->ack_done,
							   WL_LATCH_SET | WL_POSTMASTER_DEATH | WL_SOCKET_WRITEABLE | WL_TIMEOUT,
							   fifo,
							   POLL_FIFO_TIMEOUT);

		/*
		 * Data is not sent out, so ack_done is not expected
		 */
		Assert(!(wr & WL_LATCH_SET));

		if (wr & WL_POSTMASTER_DEATH)
			proc_exit(0);
	}
}

void SendTupleSlot(TupleTableSlot *slot)
{
	char	cmd;

	cmd = 'T';

	if (Gp_endpoint_role != EPR_SENDER)
		ep_log(ERROR, "%s could not send tuple", endpoint_role_to_string(Gp_endpoint_role));

	slot_getallattrs(slot);
	retry_write(s_fifoConnState->fifo, &cmd, 1);
	retry_write(s_fifoConnState->fifo, (char *) slot->PRIVATE_tts_values,
				 sizeof(Datum) * slot->PRIVATE_tts_nvalid);
	retry_write(s_fifoConnState->fifo, (char *) slot->PRIVATE_tts_isnull,
				 sizeof(bool) * slot->PRIVATE_tts_nvalid);
}

static void
retry_read(int fifo, char *data, int len)
{
	int				rdRet;
	int				curr = 0;
	struct pollfd	fds;

	ep_log(LOG, "Reading data(%d)\n", len);

	fds.fd = fifo;
	fds.events = POLLIN;

	while (len > 0)
	{
		int 			pollRet;
	
		do
		{
			CHECK_FOR_INTERRUPTS();
			pollRet = poll(&fds, 1, POLL_FIFO_TIMEOUT);
		}
		while (pollRet == 0 || (pollRet < 0 && (errno == EINTR || errno == EAGAIN)));

		if (pollRet < 0)
			ep_log(ERROR, "poll failed during read pipe:%m");

		rdRet = read(fifo, &data[curr], len);
		if (rdRet >= 0)
		{
			ep_log(LOG, "data read %d bytes", len);
			curr += rdRet;
			len -= rdRet;
		}
		else if (rdRet == 0 && errno == EINTR)
			continue;
		else if (errno == EAGAIN || errno == EWOULDBLOCK)
			continue;
		else
			ep_log(ERROR, "could not read from fifo:%m");
	}
}

TupleTableSlot* RecvTupleSlot()
{
	char			cmd;
	int				fifo;
	TupleTableSlot	*slot;

	Assert(s_fifoConnState);
	Assert(s_resultTupleSlot);

	ExecClearTuple(s_resultTupleSlot);

	fifo = s_fifoConnState->fifo;
	slot = s_resultTupleSlot;

	while (true)
	{
		retry_read(s_fifoConnState->fifo, &cmd, 1);

		if (cmd == 'F')
		{
			s_needAck = true;
			return NULL;
		}

		Assert(cmd == 'T');

		retry_read(s_fifoConnState->fifo, (char *) slot_get_values(slot),
			   	 	slot->tts_tupleDescriptor->natts * sizeof(Datum));
		retry_read(s_fifoConnState->fifo, (char *) slot_get_isnull(slot),
					slot->tts_tupleDescriptor->natts * sizeof(bool));
		ExecStoreVirtualTuple(slot);

		return slot;
	}
}

static void sender_finish()
{
	char	cmd = 'F';

	retry_write(s_fifoConnState->fifo, &cmd, 1);

	while (true)
	{
		int		wr;

		CHECK_FOR_INTERRUPTS();

		wr = WaitLatch(&mySharedEndPoint->ack_done,
					   WL_LATCH_SET | WL_POSTMASTER_DEATH | WL_TIMEOUT,
					   POLL_FIFO_TIMEOUT);

		if (wr & WL_TIMEOUT)
			continue;

		if (wr & WL_POSTMASTER_DEATH)
		{
			CloseConn();
			proc_exit(0);
		}

		Assert (wr & WL_LATCH_SET);
		break;
	}
}

static void receiver_finish()
{
	ep_log(LOG, "Finish receive.\n");
}

void FinishConn(void)
{
	switch (Gp_endpoint_role)
	{
	case EPR_SENDER:
		sender_finish();
		break;
	case EPR_RECEIVER:
		receiver_finish();
		break;
	default:
		ep_log(ERROR, "none end point role");
	}

	s_fifoConnState->finished = true;
}

static void
sender_close()
{
	char	fifo_name[MAX_FIFO_NAME_SIZE];

	snprintf(fifo_name, sizeof(fifo_name), FIFO_NAME_PATTERN, GpIdentity.segindex, Gp_token);

	Assert(s_fifoConnState->fifo > 0);

	if (s_fifoConnState->fifo > 0 && close(s_fifoConnState->fifo) < 0)
		ep_log(ERROR, "failed to close fifo %s:%m", fifo_name);

	s_fifoConnState->fifo = -1;

	if (!s_fifoConnState->created)
		return;

	if (unlink(fifo_name) < 0)
		ep_log(ERROR, "failed to unlink fifo %s:%m", fifo_name);

	s_fifoConnState->created = false;
}

static void
receiver_close()
{
	if (close(s_fifoConnState->fifo) < 0)
		ep_log(ERROR, "failed to close fifo:%m");

	s_fifoConnState->fifo = -1;
}

void CloseConn(void)
{
	Assert(s_fifoConnState);

	if (!s_fifoConnState->finished)
		ep_log(ERROR, "not finished");

	check_gp_token_valid();

	switch (Gp_endpoint_role)
	{
	case EPR_SENDER:
		sender_close();
		break;
	case EPR_RECEIVER:
		receiver_close();
		break;
	default:
		ep_log(ERROR, "none end point role");
	}

	pfree(s_fifoConnState);
	s_fifoConnState = NULL;
}

void AbortEndPoint(void)
{
	s_inAbort = true;

	switch (Gp_endpoint_role)
	{
	case EPR_SENDER:
		if (s_fifoConnState)
			sender_close();
		FreeEndPoint();
		break;
	case EPR_RECEIVER:
		if (s_fifoConnState)
			receiver_close();
		DetachEndPoint();
		break;
	default:
		break;
	}

	s_inAbort = false;
	Gp_token = InvalidToken;
	Gp_endpoint_role = EPR_NONE;
}
