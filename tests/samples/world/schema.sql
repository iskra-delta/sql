CREATE DATABASE world;
USE world;
CREATE TABLE city (
    id NUMERIC(4),
    name CHAR(34),
    countrycode CHAR(3),
    district CHAR(22),
    population NUMERIC(8)
);
CREATE TABLE country (
    code CHAR(3),
    name CHAR(44),
    continent CHAR(13),
    region CHAR(25),
    surfacearea CHAR(11),
    indepyear NUMERIC(5),
    population NUMERIC(10),
    lifeexpecta CHAR(4),
    gnp CHAR(10),
    gnpold CHAR(10),
    localname CHAR(44),
    governmentf CHAR(44),
    headofstate CHAR(36),
    capital NUMERIC(4),
    code2 CHAR(2)
);
CREATE TABLE countrylanguage (
    countrycode CHAR(3),
    language CHAR(25),
    isofficial LOGICAL,
    percentage CHAR(5)
);
