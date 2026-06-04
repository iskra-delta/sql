CREATE DATABASE chinook;
USE chinook;
CREATE TABLE album (
    albumid NUMERIC(3),
    title CHAR(95),
    artistid NUMERIC(3)
);
CREATE TABLE artist (
    artistid NUMERIC(3),
    name CHAR(85)
);
CREATE TABLE customer (
    customerid NUMERIC(2),
    firstname CHAR(10),
    lastname CHAR(13),
    company CHAR(49),
    address CHAR(41),
    city CHAR(21),
    state CHAR(6),
    country CHAR(14),
    postalcode CHAR(10),
    phone CHAR(19),
    fax CHAR(18),
    email CHAR(29),
    supportreid NUMERIC(1)
);
CREATE TABLE employee (
    employeeid NUMERIC(1),
    lastname CHAR(8),
    firstname CHAR(8),
    title CHAR(19),
    reportsto NUMERIC(1),
    birthdate DATE,
    hiredate DATE,
    address CHAR(27),
    city CHAR(10),
    state CHAR(2),
    country CHAR(6),
    postalcode CHAR(7),
    phone CHAR(17),
    fax CHAR(17),
    email CHAR(24)
);
CREATE TABLE genre (
    genreid NUMERIC(2),
    name CHAR(18)
);
CREATE TABLE invoice (
    invoiceid NUMERIC(3),
    customerid NUMERIC(2),
    invoicedate DATE,
    billingaddr CHAR(41),
    billingcity CHAR(21),
    billingstat CHAR(6),
    billingcoun CHAR(14),
    billingpost CHAR(10),
    totalc NUMERIC(4)
);
CREATE TABLE invoice_line (
    invoiceliid NUMERIC(4),
    invoiceid NUMERIC(3),
    trackid NUMERIC(4),
    unitpricec NUMERIC(3),
    quantity NUMERIC(1)
);
CREATE TABLE media_type (
    mediatypeid NUMERIC(1),
    name CHAR(27)
);
CREATE TABLE playlist (
    playlistid NUMERIC(2),
    name CHAR(26)
);
CREATE TABLE playlist_track (
    playlistid NUMERIC(2),
    trackid NUMERIC(4)
);
CREATE TABLE track (
    trackid NUMERIC(4),
    name CHAR(123),
    albumid NUMERIC(3),
    mediatypeid NUMERIC(1),
    genreid NUMERIC(2),
    composer CHAR(188),
    millisecond NUMERIC(7),
    bytes NUMERIC(10),
    unitpricec NUMERIC(3)
);
