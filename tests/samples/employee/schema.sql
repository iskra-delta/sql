CREATE DATABASE employee;
USE employee;
CREATE TABLE department (
    deptno CHAR(4),
    deptname CHAR(18)
);
CREATE TABLE dept_emp (
    empno NUMERIC(5),
    deptno CHAR(4),
    fromdate DATE,
    todate DATE
);
CREATE TABLE dept_manager (
    empno NUMERIC(5),
    deptno CHAR(4),
    fromdate DATE,
    todate DATE
);
CREATE TABLE employee (
    empno NUMERIC(5),
    birthdate DATE,
    firstname CHAR(13),
    lastname CHAR(14),
    gender CHAR(1),
    hiredate DATE
);
CREATE TABLE expected_value (
    tablename CHAR(12),
    recs NUMERIC(4),
    crcmd5 CHAR(32)
);
CREATE TABLE found_value (
    tablename CHAR(1),
    recs NUMERIC(1),
    crcmd5 CHAR(1)
);
CREATE TABLE salary (
    empno NUMERIC(5),
    amount NUMERIC(6),
    fromdate DATE,
    todate DATE
);
CREATE TABLE tchecksum (
    chk CHAR(1)
);
CREATE TABLE title (
    empno NUMERIC(5),
    title CHAR(18),
    fromdate DATE,
    todate DATE
);
