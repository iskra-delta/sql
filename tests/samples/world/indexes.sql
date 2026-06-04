USE world;
CREATE INDEX idxcityid ON city (id);
CREATE INDEX idxcitycountryco ON city (countrycode);
CREATE INDEX idxcityname ON city (name);
CREATE INDEX idxcountrycode ON country (code);
CREATE INDEX idxcountryname ON country (name);
CREATE INDEX idxcountrylangua ON countrylanguage (countrycode);
