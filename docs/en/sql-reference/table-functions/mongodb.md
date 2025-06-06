---
description: 'Allows `SELECT` queries to be performed on data that is stored on a
  remote MongoDB server.'
sidebar_label: 'mongodb'
sidebar_position: 135
slug: /sql-reference/table-functions/mongodb
title: 'mongodb'
---

# mongodb Table Function

Allows `SELECT` queries to be performed on data that is stored on a remote MongoDB server.

## Syntax {#syntax}

```sql
mongodb(host:port, database, collection, user, password, structure[, options[, oid_columns]])
```

## Arguments {#arguments}

| Argument      | Description                                                                                            |
|---------------|--------------------------------------------------------------------------------------------------------|
| `host:port`   | MongoDB server address.                                                                                |
| `database`    | Remote database name.                                                                                  |
| `collection`  | Remote collection name.                                                                                |
| `user`        | MongoDB user.                                                                                          |
| `password`    | User password.                                                                                         |
| `structure`   | The schema for the ClickHouse table returned from this function.                                       |
| `options`     | MongoDB connection string options (optional parameter).                                                |
| `oid_columns` | Comma-separated list of columns that should be treated as `oid` in the WHERE clause. `_id` by default. |

:::tip
If you are using the MongoDB Atlas cloud offering please add these options:

```ini
'connectTimeoutMS=10000&ssl=true&authSource=admin'
```
:::

You can also connect by URI:

```sql
mongodb(uri, collection, structure[, oid_columns])
```

| Argument      | Description                                                                                            |
|---------------|--------------------------------------------------------------------------------------------------------|
| `uri`         | Connection string.                                                                                     |
| `collection`  | Remote collection name.                                                                                |
| `structure`   | The schema for the ClickHouse table returned from this function.                                       |
| `oid_columns` | Comma-separated list of columns that should be treated as `oid` in the WHERE clause. `_id` by default. |

## Returned value {#returned_value}

A table object with the same columns as the original MongoDB table.

## Examples {#examples}

Suppose we have a collection named `my_collection` defined in a MongoDB database named `test`, and we insert a couple of documents:

```sql
db.createUser({user:"test_user",pwd:"password",roles:[{role:"readWrite",db:"test"}]})

db.createCollection("my_collection")

db.my_collection.insertOne(
    { log_type: "event", host: "120.5.33.9", command: "check-cpu-usage -w 75 -c 90" }
)

db.my_collection.insertOne(
    { log_type: "event", host: "120.5.33.4", command: "system-check"}
)
```

Let's query the collection using the `mongodb` table function:

```sql
SELECT * FROM mongodb(
    '127.0.0.1:27017',
    'test',
    'my_collection',
    'test_user',
    'password',
    'log_type String, host String, command String',
    'connectTimeoutMS=10000'
)
```

or:

```sql
SELECT * FROM mongodb(
    'mongodb://test_user:password@127.0.0.1:27017/test?connectionTimeoutMS=10000',
    'my_collection',
    'log_type String, host String, command String'
)
```

## Related {#related}

- [The `MongoDB` table engine](engines/table-engines/integrations/mongodb.md)
- [Using MongoDB as a dictionary source](sql-reference/dictionaries/index.md#mongodb)
