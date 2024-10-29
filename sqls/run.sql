drop table if exists t1;

create table t1 (seq sequence varchar(10));

drop table if exists t2;
create table t2 (vec vector varchar(10));

insert into t2 values ({'a', 'b', 'c'});
