# chinook

- Source: [Chinook sample database](https://github.com/lerocha/chinook-database/releases/download/v1.4.5/Chinook_Sqlite.sqlite)
- License / provenance: See lerocha/chinook-database LICENSE.md

## Usage

```sh
./bin/sql tests/samples/chinook
```

Then run:

```sql
USE chinook;
SHOW VIEWS;
```

## Notes

- Invoice and track price fields are stored as integer cents.
- Timestamp values were normalized to YYYYMMDD.
- `bootstrap.sql` creates the database slot and catalogs.
- `schema.sql` is the imported logical schema reference.
- Table names were shortened to fit the shell identifier limit.
- DBF field names were shortened to 11 bytes when needed.

## Tables

| Source | Imported | Rows | Indexed columns |
|---|---|---:|---|
| `Album` | `album` | 347 | albumid, artistid, title |
| `Artist` | `artist` | 275 | artistid, name |
| `Customer` | `customer` | 59 | customerid, supportreid, lastname |
| `Employee` | `employee` | 8 | employeeid, reportsto, lastname, title |
| `Genre` | `genre` | 25 | genreid, name |
| `Invoice` | `invoice` | 412 | invoiceid, customerid |
| `InvoiceLine` | `invoice_line` | 2240 | invoiceliid, invoiceid, trackid |
| `MediaType` | `media_type` | 5 | mediatypeid, name |
| `Playlist` | `playlist` | 18 | playlistid, name |
| `PlaylistTrack` | `playlist_track` | 8715 | playlistid, trackid |
| `Track` | `track` | 3503 | trackid, albumid, mediatypeid, genreid |

## Column Mapping

### `album`

| Source | Imported | Type |
|---|---|---|
| `AlbumId` | `albumid` | `NUMERIC(3)` |
| `Title` | `title` | `CHAR(95)` |
| `ArtistId` | `artistid` | `NUMERIC(3)` |

### `artist`

| Source | Imported | Type |
|---|---|---|
| `ArtistId` | `artistid` | `NUMERIC(3)` |
| `Name` | `name` | `CHAR(85)` |

### `customer`

| Source | Imported | Type |
|---|---|---|
| `CustomerId` | `customerid` | `NUMERIC(2)` |
| `FirstName` | `firstname` | `CHAR(10)` |
| `LastName` | `lastname` | `CHAR(13)` |
| `Company` | `company` | `CHAR(49)` |
| `Address` | `address` | `CHAR(41)` |
| `City` | `city` | `CHAR(21)` |
| `State` | `state` | `CHAR(6)` |
| `Country` | `country` | `CHAR(14)` |
| `PostalCode` | `postalcode` | `CHAR(10)` |
| `Phone` | `phone` | `CHAR(19)` |
| `Fax` | `fax` | `CHAR(18)` |
| `Email` | `email` | `CHAR(29)` |
| `SupportRepId` | `supportreid` | `NUMERIC(1)` |

### `employee`

| Source | Imported | Type |
|---|---|---|
| `EmployeeId` | `employeeid` | `NUMERIC(1)` |
| `LastName` | `lastname` | `CHAR(8)` |
| `FirstName` | `firstname` | `CHAR(8)` |
| `Title` | `title` | `CHAR(19)` |
| `ReportsTo` | `reportsto` | `NUMERIC(1)` |
| `BirthDate` | `birthdate` | `DATE` |
| `HireDate` | `hiredate` | `DATE` |
| `Address` | `address` | `CHAR(27)` |
| `City` | `city` | `CHAR(10)` |
| `State` | `state` | `CHAR(2)` |
| `Country` | `country` | `CHAR(6)` |
| `PostalCode` | `postalcode` | `CHAR(7)` |
| `Phone` | `phone` | `CHAR(17)` |
| `Fax` | `fax` | `CHAR(17)` |
| `Email` | `email` | `CHAR(24)` |

### `genre`

| Source | Imported | Type |
|---|---|---|
| `GenreId` | `genreid` | `NUMERIC(2)` |
| `Name` | `name` | `CHAR(18)` |

### `invoice`

| Source | Imported | Type |
|---|---|---|
| `InvoiceId` | `invoiceid` | `NUMERIC(3)` |
| `CustomerId` | `customerid` | `NUMERIC(2)` |
| `InvoiceDate` | `invoicedate` | `DATE` |
| `BillingAddress` | `billingaddr` | `CHAR(41)` |
| `BillingCity` | `billingcity` | `CHAR(21)` |
| `BillingState` | `billingstat` | `CHAR(6)` |
| `BillingCountry` | `billingcoun` | `CHAR(14)` |
| `BillingPostalCode` | `billingpost` | `CHAR(10)` |
| `Total` | `totalc` | `NUMERIC(4)` |

### `invoice_line`

| Source | Imported | Type |
|---|---|---|
| `InvoiceLineId` | `invoiceliid` | `NUMERIC(4)` |
| `InvoiceId` | `invoiceid` | `NUMERIC(3)` |
| `TrackId` | `trackid` | `NUMERIC(4)` |
| `UnitPrice` | `unitpricec` | `NUMERIC(3)` |
| `Quantity` | `quantity` | `NUMERIC(1)` |

### `media_type`

| Source | Imported | Type |
|---|---|---|
| `MediaTypeId` | `mediatypeid` | `NUMERIC(1)` |
| `Name` | `name` | `CHAR(27)` |

### `playlist`

| Source | Imported | Type |
|---|---|---|
| `PlaylistId` | `playlistid` | `NUMERIC(2)` |
| `Name` | `name` | `CHAR(26)` |

### `playlist_track`

| Source | Imported | Type |
|---|---|---|
| `PlaylistId` | `playlistid` | `NUMERIC(2)` |
| `TrackId` | `trackid` | `NUMERIC(4)` |

### `track`

| Source | Imported | Type |
|---|---|---|
| `TrackId` | `trackid` | `NUMERIC(4)` |
| `Name` | `name` | `CHAR(123)` |
| `AlbumId` | `albumid` | `NUMERIC(3)` |
| `MediaTypeId` | `mediatypeid` | `NUMERIC(1)` |
| `GenreId` | `genreid` | `NUMERIC(2)` |
| `Composer` | `composer` | `CHAR(188)` |
| `Milliseconds` | `millisecond` | `NUMERIC(7)` |
| `Bytes` | `bytes` | `NUMERIC(10)` |
| `UnitPrice` | `unitpricec` | `NUMERIC(3)` |
