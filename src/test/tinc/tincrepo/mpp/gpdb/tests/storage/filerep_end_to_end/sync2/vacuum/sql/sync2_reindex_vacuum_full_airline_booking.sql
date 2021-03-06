-- start_ignore
SET gp_create_table_random_default_distribution=off;
-- end_ignore
reindex database gptest;
vacuum full;

create table sync2_reindex_vacuum_full_test1 (i int) partition by range(i) (start(1) end(10) every(1));
create table sync2_reindex_vacuum_full_test2 (i int) partition by range(i) (start(1) end(10) every(1));
alter table sync2_reindex_vacuum_full_test2 drop partition for (1);
alter table sync2_reindex_vacuum_full_test2 drop partition for (2);
alter table sync2_reindex_vacuum_full_test2 drop partition for (3);
alter table sync2_reindex_vacuum_full_test2 drop partition for (4);
alter table sync2_reindex_vacuum_full_test2 drop partition for (5);
drop table sync2_reindex_vacuum_full_test1;
create table sync2_reindex_vacuum_full_test3(i int);

create table sync2_reindex_vacuum_full_t1heap (c1 int, c2 char(100)) distributed by (c1);
insert into sync2_reindex_vacuum_full_t1heap select generate_series(1,100000), 'abcdefg';
insert into sync2_reindex_vacuum_full_t1heap values (112232, 'xyz');

delete from sync2_reindex_vacuum_full_t1heap where c1 not in (1, 12, 45, 46, 112232);

reindex database gptest;
vacuum full;

