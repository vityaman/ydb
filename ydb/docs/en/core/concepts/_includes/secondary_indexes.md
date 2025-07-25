# Secondary Indexes

{{ ydb-short-name }} automatically creates a primary key index, which is why selection by primary key is always efficient, affecting only the rows needed. Selections by criteria applied to one or more non-key columns typically result in a full table scan. To make these selections efficient, use _secondary indexes_.

The current version of {{ ydb-short-name }} implements _synchronous_ and _asynchronous_ global secondary indexes. Each index is a hidden table that is updated:

* For synchronous indexes: Transactionally when the main table changes.
* For asynchronous indexes: In the background while getting the necessary changes from the main table.

When a user sends an SQL query to insert, modify, or delete data, the database transparently generates commands to modify the index table. A table may have multiple secondary indexes. An index may include multiple columns, and the sequence of columns in an index matters. A single column may be included in multiple indexes. In addition to the specified columns, every index implicitly stores the table primary key columns to enable navigation from an index record to the table row.

## Synchronous Secondary Index {#sync}

A synchronous index is updated simultaneously with the table that it indexes. This index ensures [strict consistency](https://en.wikipedia.org/wiki/Consistency_model) through [distributed transactions](../transactions.md#distributed-tx). While reads and blind writes to a table with no index can be performed without a planning stage, significantly reducing delays, such optimization is impossible when writing data to a table with a synchronous index.

## Asynchronous Secondary Index {#async}

Unlike a synchronous index, an asynchronous index doesn't use distributed transactions. Instead, it receives changes from an indexed table in the background. Write transactions to a table using this index are performed with no planning overheads due to reduced guarantees: an asynchronous index provides [eventual consistency](https://en.wikipedia.org/wiki/Eventual_consistency), but no strict consistency. You can only use asynchronous indexes in read transactions in [Stale Read Only](../transactions.md#modes) mode.

## Covering Secondary Index {#covering}

You can copy the contents of columns into a covering index. This eliminates the need to read data from the main table when performing reads by index and significantly reduces delays. At the same time, such denormalization leads to increased usage of disk space and may slow down inserts and updates due to the need for additional data copying.

## Unique Secondary Index {#unique}

This type of index enforces unique constraint behavior and, like other indexes, allows efficient point lookup queries. {{ ydb-short-name }} uses it to perform additional checks, ensuring that each distinct value in the indexed column set appears in the table no more than once. If a modifying query violates the constraint, it will be aborted with a `PRECONDITION_FAILED` status. Therefore, client code must be prepared to handle this status.

A unique secondary index is a synchronous index, so the update process is the same as in the [Synchronous Secondary Index](#sync) section described above from a transaction perspective.

### Limitations

Currently, a unique index cannot be added to an existing table.

## Vector Index

[Vector Index](../../dev/vector-indexes.md) is a special type of secondary index.

Unlike secondary indexes, which optimize equality or range searches, vector indexes allow [vector search](../vector_search.md) based on distance or similarity functions.

### Creating a Secondary Index Online {#index-add}

{{ ydb-short-name }} lets you create new and delete existing secondary indexes without stopping the service. For a single table, you can only create one index at a time.

Online index creation consists of the following steps:

1. Taking a snapshot of a data table and creating an index table marked that writes are available.

   After this step, write transactions are distributed, writing to the main table and the index, respectively. The index is not yet available to the user.

1. Reading the snapshot of the main table and writing data to the index.

   "Writes to the past" are implemented: situations where data updates in step 1 change the data written in step 2 are resolved.

1. Publishing the results and deleting the snapshot.

   The index is ready to use.

{% note info %}

The user account that is used to create a secondary index must have the following [permissions](../../yql/reference/syntax/grant.md#permissions-list):

* `ydb.generic.read`
* `ydb.generic.write`

{% endnote %}

Possible impact on user transactions:

* There may be an increase in delays because transactions are now distributed (when creating a synchronous index).
* There may be an enhanced background of `OVERLOADED` errors because index table automatic shard splitting is actively running during data writes.

The rate of data writes is selected to minimize their impact on user transactions. To quickly complete the operation, we recommend running the online creation of a secondary index when the user load is minimum.

Creating an index is an asynchronous operation. If the client-server connection is interrupted after the operation has started, index building continues. You can manage asynchronous operations using the {{ ydb-short-name }} CLI.

## Creating and Deleting Secondary Indexes {#ddl}

A secondary index can be:

- Created when creating a table with the YQL [`CREATE TABLE`](../../yql/reference/syntax/create_table/index.md) statement.
- Added to an existing table with the YQL [`ALTER TABLE`](../../yql/reference/syntax/alter_table/index.md) statement or the YDB CLI [`table index add`](../../reference/ydb-cli/commands/secondary_index.md#add) command.
- Deleted from an existing table with the YQL [`ALTER TABLE`](../../yql/reference/syntax/alter_table/index.md) statement or the YDB CLI [`table index drop`](../../reference/ydb-cli/commands/secondary_index.md#drop) command.
- Deleted together with the table using the YQL [`DROP TABLE`](../../yql/reference/syntax/drop_table.md) statement or the YDB CLI `table drop` command.

## Using Secondary Indexes {#use}

For detailed information on using secondary indexes in applications, refer to the [relevant article](../../dev/secondary-indexes.md) in the documentation section for developers.
