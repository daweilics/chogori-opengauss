/*
MIT License

Copyright(c) 2022 Futurewei Cloud

    Permission is hereby granted,
    free of charge, to any person obtaining a copy of this software and associated documentation files(the "Software"), to deal in the Software without restriction, including without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and / or sell copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to the following conditions :

    The above copyright notice and this permission notice shall be included in all copies
    or
    substantial portions of the Software.

    THE SOFTWARE IS PROVIDED "AS IS",
    WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
    FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.IN NO EVENT SHALL THE
    AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
    DAMAGES OR OTHER
    LIABILITY,
    WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
    OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
    SOFTWARE.
*/

// When we mix certain C++ standard lib code and pg code there seems to be a macro conflict that
// will cause compiler errors in libintl.h. Including as the first thing fixes this.
#include <libintl.h>
#include "postgres.h"
#include "access/k2/pg_gate_api.h"
#include "access/k2/k2pg_aux.h"
#include "access/sysattr.h"
#include "catalog/pg_type.h"
#include "fmgr/fmgr_comp.h"

#include "storage.h"

#include <skvhttp/dto/SKVRecord.h>

namespace k2pg {
namespace gate {

int K2CodeToPGCode(int k2code) {
    switch (k2code) {
        case 200: // OK Codes
        case 201:
        case 202:
            return ERRCODE_SUCCESSFUL_COMPLETION;
        case 400: // Bad request
            return ERRCODE_INTERNAL_ERROR;
        case 403: // Forbidden, used to indicate AbortRequestTooOld in K23SI
            return ERRCODE_SNAPSHOT_INVALID;
        case 404: // Not found
            return ERRCODE_SUCCESSFUL_COMPLETION;
        case 405: // Not allowed, indicates a bug in K2 usage or operation
        case 406: // Not acceptable, used to indicate BadFilterExpression
        case 408: // Timeout
            return ERRCODE_INTERNAL_ERROR;
        case 409: // Conflict, used to indicate K23SI transaction conflicts
            return ERRCODE_T_R_SERIALIZATION_FAILURE;
        case 410: // Gone, indicates a partition map error
            return ERRCODE_INTERNAL_ERROR;
        case 412: // Precondition failed, indicates a failed K2 insert operation
            return ERRCODE_UNIQUE_VIOLATION;
        case 422: // Unprocessable entity, BadParameter in K23SI, indicates a bug in usage or operation
        case 500: // Internal error, indicates a bug in K2 code
        case 503: // Service unavailable, indicates a partition is not assigned
        default:
            return ERRCODE_INTERNAL_ERROR;
    }

    return ERRCODE_INTERNAL_ERROR;
}

K2PgStatus K2StatusToK2PgStatus(skv::http::Status&& status) {
    K2PgStatus out_status{
        .pg_code = K2CodeToPGCode(status.code),
        .k2_code = status.code,
        .msg = std::move(status.message),
        .detail = ""
    };

    return out_status;
}


// These are types that we can push down filter operations to K2, so when we convert them we want to
// strip out the Datum headers
bool isStringType(Oid oid) {
    return (oid == VARCHAROID || oid == BPCHAROID || oid == TEXTOID || oid == CLOBOID || oid == BYTEAOID);
}

// Type to size association taken from MOT column.cpp. Note that this does not determine whether we can use the type as a key or for pushdown, only that it will fit in a K2 native type
bool is1ByteIntType(Oid oid) {
    return (oid == CHAROID || oid == INT1OID);
}

bool is2ByteIntType(Oid oid) {
    return (oid == INT2OID);
}

bool is4ByteIntType(Oid oid) {
    return (oid == INT4OID || oid == DATEOID);
}

bool is8ByteIntType(Oid oid) {
    return (oid == INT8OID || oid == TIMESTAMPOID || oid == TIMESTAMPTZOID || oid == TIMEOID || oid == INTERVALOID);
}

// A class for untoasted datums so that freeing data can be done by the destructor and exception-safe
class UntoastedDatum {
public:
    bytea* untoasted;
    Datum datum;
    UntoastedDatum(Datum d) : datum(d) {
        untoasted = DatumGetByteaP(datum);
    }

    ~UntoastedDatum() {
        if ((char*)datum != (char*)untoasted) {
            pfree(untoasted);
        }
    }
};

K2PgStatus getSKVBuilder(K2PgOid database_oid, K2PgOid table_oid, std::shared_ptr<k2pg::catalog::SqlCatalogClient> catalog,
                         std::unique_ptr<skv::http::dto::SKVRecordBuilder>& builder) {
    std::string collectionName;
    std::string schemaName;

    skv::http::Status catalog_status = catalog->GetCollectionNameAndSchemaName(database_oid, table_oid, collectionName, schemaName);
    if (!catalog_status.is2xxOK()) {
        return K2StatusToK2PgStatus(std::move(catalog_status));
    }

    auto [status, schema] = k2pg::TXMgr.GetSchema(collectionName, schemaName);
    if (!status.is2xxOK()) {
        return K2StatusToK2PgStatus(std::move(status));
    }

    builder = std::make_unique<skv::http::dto::SKVRecordBuilder>(collectionName, schema);
    return K2PgStatus::OK;
}

// May throw if there is a schema mismatch bug
void serializePGConstToK2SKV(skv::http::dto::SKVRecordBuilder& builder, K2PgConstant constant) {
    // Three different types of constants to handle:
    // 1: String-like types that we can push down operations into K2.
    // 2: Numeric types that fit in a K2 equivalent.
    // 3: Arbitrary binary types that we store the datum contents directly including header

    if (constant.is_null) {
        builder.serializeNull();
        return;
    }
    else if (isStringType(constant.type_id)) {
        // Borrowed from MOT. This handles stripping the datum header for toasted or non-toasted data
        UntoastedDatum data = UntoastedDatum(constant.datum);
        size_t size = VARSIZE(data.untoasted);  // includes header len VARHDRSZ
        char* src = VARDATA(data.untoasted);
        builder.serializeNext<std::string>(std::string(src, size - VARHDRSZ));
    }
    else if (is1ByteIntType(constant.type_id)) {
        int8_t byte = (int8_t)(((uintptr_t)(constant.datum)) & 0x000000ff);
        builder.serializeNext<int16_t>(byte);
    }
    else if (is2ByteIntType(constant.type_id)) {
        int16_t byte2 = (int16_t)(((uintptr_t)(constant.datum)) & 0x0000ffff);
        builder.serializeNext<int16_t>(byte2);
    }
    else if (is4ByteIntType(constant.type_id)) {
        int32_t byte4 = (int32_t)(((uintptr_t)(constant.datum)) & 0xffffffff);
        builder.serializeNext<int32_t>(byte4);
    }
    else if (is8ByteIntType(constant.type_id)) {
        int64_t byte8 = (int64_t)constant.datum;
        builder.serializeNext<int64_t>(byte8);
    }
    else if (constant.type_id == FLOAT4OID) {
        uint32_t fbytes = (uint32_t)(((uintptr_t)(constant.datum)) & 0xffffffff);
        // We don't want to convert, we want to treat the bytes directly as the float's bytes
        float fval = *(float*)&fbytes;
        builder.serializeNext<float>(fval);
    }
    else if (constant.type_id == FLOAT8OID) {
        // We don't want to convert, we want to treat the bytes directly as the double's bytes
        double dval = *(double*)&(constant.datum);
        builder.serializeNext<double>(dval);
    } else {
        // Anything else we treat as opaque bytes and include the datum header
        UntoastedDatum data = UntoastedDatum(constant.datum);
        size_t size = VARSIZE(data.untoasted);  // includes header len VARHDRSZ
        builder.serializeNext<std::string>(std::string((char*)data.untoasted, size));
    }
}

skv::http::dto::SKVRecord tupleIDDatumToSKVRecord(Datum tuple_id, std::string collection, std::shared_ptr<skv::http::dto::Schema> schema) {
    UntoastedDatum data = UntoastedDatum(tuple_id);
    size_t size = VARSIZE(data.untoasted) - VARHDRSZ;
    char* src = VARDATA(data.untoasted);
    // No-op deleter. Data is owned by PG heap and we will not access it outside of this function
    skv::http::Binary binary(src, size, [] () {});
    skv::http::MPackReader reader(binary);
    skv::http::dto::SKVRecord::Storage storage;
    reader.read(storage);
    return skv::http::dto::SKVRecord(collection, schema, std::move(storage), true);
}

K2PgStatus makeSKVBuilderWithKeysSerialized(K2PgOid database_oid, K2PgOid table_oid,
                                           std::shared_ptr<k2pg::catalog::SqlCatalogClient> catalog, const std::vector<K2PgAttributeDef>& columns,
                                           std::unique_ptr<skv::http::dto::SKVRecordBuilder>& builder) {
    skv::http::dto::SKVRecord record;
    bool use_tupleID = false;

    // Check if we have the virtual tupleID column and if so deserialize the datum into a SKVRecord
    for (auto& attribute : columns) {
        if (attribute.attr_num == K2PgTupleIdAttributeNumber) {
            std::unique_ptr<skv::http::dto::SKVRecordBuilder> builder;
            K2PgStatus status = getSKVBuilder(database_oid, table_oid, catalog, builder);
            if (status.pg_code != ERRCODE_SUCCESSFUL_COMPLETION) {
                return status;
            }

        record = tupleIDDatumToSKVRecord(attribute.value.datum, builder->getCollectionName(), builder->getSchema());
        use_tupleID = true;
        }
    }

    // Get a SKVBuilder
    K2PgStatus status = getSKVBuilder(database_oid, table_oid, catalog, builder);
    if (status.pg_code != ERRCODE_SUCCESSFUL_COMPLETION) {
        return status;
    }

    // If we have a tupleID just need to copy fields from the record to a builder and we are done
    if (use_tupleID) {
        K2PgStatus status = serializeKeysFromSKVRecord(record, *builder);
        if (status.pg_code != ERRCODE_SUCCESSFUL_COMPLETION) {
            return status;
        }

        return K2PgStatus::OK;
    }


    // If we get here it is because we did not have a tupleID, so prepare to serialize keys from the provided columns

    // Get attribute to SKV offset mapping
    std::unordered_map<int, uint32_t> attr_to_offset;
    skv::http::Status catalog_status = catalog->GetAttrNumToSKVOffset(database_oid, table_oid, attr_to_offset);
    if (!catalog_status.is2xxOK()) {
        return K2StatusToK2PgStatus(std::move(catalog_status));
    }


    // Create SKV offset to Pg Constant mappping
    std::unordered_map<int, K2PgConstant> attr_map;
    for (size_t i=0; i < columns.size(); ++i) {
        auto it = attr_to_offset.find(columns[i].attr_num);
        if (it != attr_to_offset.end()) {
            attr_map[it->second] = columns[i].value;
        }
    }

    // Determine table id and index id to use as first two fields in SKV record.
    // The passed in table id may or may not be a secondary index
    uint32_t base_table_oid = 0;
    catalog_status = catalog->GetBaseTableOID(database_oid, table_oid, base_table_oid);
    if (!catalog_status.is2xxOK()) {
        return K2StatusToK2PgStatus(std::move(catalog_status));
    }
    uint32_t index_id = base_table_oid == table_oid ? 0 : table_oid;

    // Last, serialize key fields into the builder
    try {
        builder->serializeNext<int32_t>(base_table_oid);
        builder->serializeNext<int32_t>(index_id);

        for (size_t i = K2_FIELD_OFFSET; i < builder->getSchema()->partitionKeyFields.size(); ++i) {
            auto it = attr_map.find(i);
            if (it == attr_map.end()) {
                builder->serializeNull();
            } else {
                serializePGConstToK2SKV(*builder, it->second);
            }
        }
    }
    catch (const std::exception& err) {
        K2PgStatus status {
            .pg_code = ERRCODE_INTERNAL_ERROR,
            .k2_code = 0,
            .msg = "Serialization error in serializePgAttributesToSKV",
            .detail = err.what()
        };

        return status;
    }

    return K2PgStatus::OK;
}

K2PgStatus serializeKeysFromSKVRecord(skv::http::dto::SKVRecord& source, skv::http::dto::SKVRecordBuilder& builder) {
    try {
        source.seekField(0);
        std::optional<int32_t> table_id = source.deserializeNext<int32_t>();
        builder.serializeNext<int32_t>(*table_id);
        std::optional<int32_t> index_id = source.deserializeNext<int32_t>();
        builder.serializeNext<int32_t>(*index_id);

        for (size_t i=K2_FIELD_OFFSET; i < source.schema->partitionKeyFields.size(); ++i) {
            source.visitNextField([&builder] (const auto& field, auto&& value) mutable {
                using T = typename std::remove_reference_t<decltype(value)>::value_type;
                if (!value.has_value()) {
                    builder.serializeNull();
                } else {
                    builder.serializeNext<T>(*value);
                }
            });
        }
    }

    catch (const std::exception& err) {
        K2PgStatus status {
            .pg_code = ERRCODE_INTERNAL_ERROR,
            .k2_code = 0,
            .msg = "Serialization error in serializeKeysFromSKVRecord",
            .detail = err.what()
        };

        return status;
    }

    return K2PgStatus::OK;
}

K2PgStatus serializePgAttributesToSKV(skv::http::dto::SKVRecordBuilder& builder, int32_t table_id, int32_t index_id,
                                      const std::vector<K2PgAttributeDef>& attrs, const std::unordered_map<int, uint32_t>& attr_num_to_index) {
    std::unordered_map<int, K2PgConstant> attr_map;
    for (size_t i=0; i < attrs.size(); ++i) {
        auto it = attr_num_to_index.find(attrs[i].attr_num);
        if (it != attr_num_to_index.end()) {
            attr_map[it->second] = attrs[i].value;
        }
    }

    try {
        builder.serializeNext<int32_t>(table_id);
        builder.serializeNext<int32_t>(index_id);

        for (size_t i = K2_FIELD_OFFSET; i < builder.getSchema()->fields.size(); ++i) {
            auto it = attr_map.find(i);
            if (it == attr_map.end()) {
                builder.serializeNull();
            } else {
                serializePGConstToK2SKV(builder, it->second);
            }
        }
    }
    catch (const std::exception& err) {
        K2PgStatus status {
            .pg_code = ERRCODE_INTERNAL_ERROR,
            .k2_code = 0,
            .msg = "Serialization error in serializePgAttributesToSKV",
            .detail = err.what()
        };

        return status;
    }

    return K2PgStatus::OK;
}

K2PgStatus makeSKVRecordFromK2PgAttributes(K2PgOid database_oid, K2PgOid table_oid,
                                           std::shared_ptr<k2pg::catalog::SqlCatalogClient> catalog, const std::vector<K2PgAttributeDef>& columns,
                                           skv::http::dto::SKVRecord& record) {
    std::unordered_map<int, uint32_t> attr_to_offset;
    skv::http::Status catalog_status = catalog->GetAttrNumToSKVOffset(database_oid, table_oid, attr_to_offset);
    if (!catalog_status.is2xxOK()) {
        return K2StatusToK2PgStatus(std::move(catalog_status));
    }

    uint32_t base_table_oid = 0;
    catalog_status = catalog->GetBaseTableOID(database_oid, table_oid, base_table_oid);
    if (!catalog_status.is2xxOK()) {
        return K2StatusToK2PgStatus(std::move(catalog_status));
    }
    uint32_t index_id = base_table_oid == table_oid ? 0 : table_oid;

    std::unique_ptr<skv::http::dto::SKVRecordBuilder> builder;
    K2PgStatus status = getSKVBuilder(database_oid, table_oid, catalog, builder);
    if (status.pg_code != ERRCODE_SUCCESSFUL_COMPLETION) {
        return status;
    }

    status = serializePgAttributesToSKV(*builder, base_table_oid, index_id, columns, attr_to_offset);
    if (status.pg_code != ERRCODE_SUCCESSFUL_COMPLETION) {
        return status;
    }

    record = builder->build();
    return K2PgStatus::OK;
}


} // k2pg ns
} // gate ns