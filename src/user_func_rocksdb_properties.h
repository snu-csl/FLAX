#ifndef _CSD_ROCKSDB_PROPERTIES_H
#define _CSD_ROCKSDB_PROPERTIES_H

#include "csd_user_func.h"
#include "user_func_rocksdb.h"

const char *kPropertiesBlockName = "rocksdb.properties";

const char *table_properties_no_filter[] = {
	"rocksdb.block.based.table.index.type",
	"rocksdb.block.based.table.prefix.filtering",
	"rocksdb.block.based.table.whole.key.filtering",
	"rocksdb.column.family.id",
	"rocksdb.column.family.name",
	"rocksdb.comparator",
	"rocksdb.compression",
	"rocksdb.compression_options",
	"rocksdb.creating.db.identity",
	"rocksdb.creating.session.identity",
	"rocksdb.creation.time",
	"rocksdb.data.size",
	"rocksdb.deleted.keys",
	"rocksdb.file.creation.time",
	"rocksdb.filter.size",
	"rocksdb.fixed.key.length",
	"rocksdb.format.version",
	"rocksdb.index.key.is.user.key",
	"rocksdb.index.size",
	"rocksdb.index.value.is.delta.encoded",
	"rocksdb.merge.operands",
	"rocksdb.merge.operator",
	"rocksdb.num.data.blocks",
	"rocksdb.num.entries",
	"rocksdb.num.filter_entries",
	"rocksdb.num.range-deletions",
	"rocksdb.oldest.key.time",
	"rocksdb.original.file.number",
	"rocksdb.prefix.extractor.name",
	"rocksdb.property.collectors",
	"rocksdb.raw.key.size",
	"rocksdb.raw.value.size",
	"rocksdb.tail.start.offset"
};

const char *table_properties_with_filter[] = {
	"rocksdb.block.based.table.index.type",
	"rocksdb.block.based.table.prefix.filtering",
	"rocksdb.block.based.table.whole.key.filtering",
	"rocksdb.column.family.id",
	"rocksdb.column.family.name",
	"rocksdb.comparator",
	"rocksdb.compression",
	"rocksdb.compression_options",
	"rocksdb.creating.db.identity",
	"rocksdb.creating.session.identity",
	"rocksdb.creation.time",
	"rocksdb.data.size",
	"rocksdb.deleted.keys",
	"rocksdb.file.creation.time",
	"rocksdb.filter.policy",
	"rocksdb.filter.size",
	"rocksdb.fixed.key.length",
	"rocksdb.format.version",
	"rocksdb.index.key.is.user.key",
	"rocksdb.index.size",
	"rocksdb.index.value.is.delta.encoded",
	"rocksdb.merge.operands",
	"rocksdb.merge.operator",
	"rocksdb.num.data.blocks",
	"rocksdb.num.entries",
	"rocksdb.num.filter_entries",
	"rocksdb.num.range-deletions",
	"rocksdb.oldest.key.time",
	"rocksdb.original.file.number",
	"rocksdb.prefix.extractor.name",
	"rocksdb.property.collectors",
	"rocksdb.raw.key.size",
	"rocksdb.raw.value.size",
	"rocksdb.tail.start.offset"
};

/* Without filter_policy (33 entries) */
char *table_char_values_no_filter[] = {
	"",
	"0",
	"1",
	"",
	"default",
	"leveldb.BytewiseComparator",
	"NoCompression",
	"window_bits=-14; level=32767; strategy=0; max_dict_bytes=0; zstd_max_train_bytes=0; enabled=0; max_dict_buffer_bytes=0; use_zstd_dict_trainer=1; ",
	"",
	"",
	"",
	"",
	"",
	"",
	"",
	"",
	"",
	"",
	"",
	"",
	"",
	"nullptr",
	"",
	"",
	"",
	"",
	"",
	"",
	"nullptr",
	"[]",
	"",
	"",
	"",
};

/* With filter_policy (34 entries) - filter_policy inserted at index 15 */
char *table_char_values_with_filter[] = {
	"",
	"0",
	"1",
	"",
	"default",
	"leveldb.BytewiseComparator",
	"NoCompression",
	"window_bits=-14; level=32767; strategy=0; max_dict_bytes=0; zstd_max_train_bytes=0; enabled=0; max_dict_buffer_bytes=0; use_zstd_dict_trainer=1; ",
	"",
	"",
	"",
	"",
	"",
	"",
	"bloomfilter",  /* filter_policy value */
	"",
	"",
	"",
	"",
	"",
	"",
	"",
	"nullptr",
	"",
	"",
	"",
	"",
	"",
	"",
	"nullptr",
	"[]",
	"",
	"",
	"",
};

uint64_t table_uint64_t_values[35];  /* Max size to accommodate both cases */

size_t difference_offset(char *last_key, char *current_key)
{
	size_t off = 0;
	const size_t len = (strlen(last_key) < strlen(current_key)) ? strlen(last_key) : strlen(current_key);
	for (; off < len; off++) {
		if (last_key[off] != current_key[off])
			break;
	}
	return off;
}

void __update_compaction_properties(struct compaction_properties *properties, struct decode_info *index_info, bool new_block)
{
	if (index_info != NULL) {
		properties->raw_key_size += index_info->non_shared; // based on the assumption that shared = 0;
		properties->raw_value_size += index_info->value_length;
		properties->num_entries++;
	}
	if (new_block) {
		properties->num_data_blocks++;
	}
}

size_t __rocksdb_write_properties_block(void *buf_file_out, size_t output_offset,
										struct compaction_properties *properties,
										struct rocksdb_host_properties_param *host_properties)
{
	char *properties_buf = buf_file_out + output_offset;
	char *start_buf = properties_buf;
	char *tmp_buf;
	char encoding_buf[10];
	size_t properties_offset = output_offset;
	char *last_key;
	size_t shared, non_shared, value_size;

	/* Select the appropriate arrays based on filter_size */
	bool has_filter = (properties->filter_size > 0);
	const char **table_properties = has_filter ? table_properties_with_filter : table_properties_no_filter;
	char **table_char_values = has_filter ? table_char_values_with_filter : table_char_values_no_filter;
	int num_properties = has_filter ?
		(sizeof(table_properties_with_filter) / sizeof(table_properties_with_filter[0])) :
		(sizeof(table_properties_no_filter) / sizeof(table_properties_no_filter[0]));

	/* Assign char values */
	// index type
	char index_type_val[4];
	PutFixed32(index_type_val, 0);
	table_char_values[0] = index_type_val;

	// db_id, db_session_id
	table_char_values[8] = host_properties->db_id;
	table_char_values[9] = host_properties->db_session_id;

	/* Assign uint64_t values - indices shift by 1 after filter_policy when filter is enabled */
	memset(table_uint64_t_values, 0, sizeof(table_uint64_t_values));
	table_uint64_t_values[10] = host_properties->creation_time;
	table_uint64_t_values[11] = properties->data_size;
	table_uint64_t_values[13] = host_properties->file_creation_time;
	if (has_filter) {
		/* With filter_policy at index 15, subsequent indices shift by 1 */
		table_uint64_t_values[15] = properties->filter_size;
		table_uint64_t_values[18] = 1;
		table_uint64_t_values[19] = properties->index_size;
		table_uint64_t_values[23] = properties->num_data_blocks;
		table_uint64_t_values[24] = properties->num_entries;
		table_uint64_t_values[25] = properties->num_entries;  /* num_filter_entries */
		table_uint64_t_values[27] = host_properties->oldest_key_time;
		table_uint64_t_values[28] = host_properties->file_number;
		table_uint64_t_values[31] = properties->raw_key_size;
		table_uint64_t_values[32] = properties->raw_value_size;
		table_uint64_t_values[33] = properties->tail_start_offset;
	} else {
		table_uint64_t_values[17] = 1;
		table_uint64_t_values[18] = properties->index_size;
		table_uint64_t_values[22] = properties->num_data_blocks;
		table_uint64_t_values[23] = properties->num_entries;
		table_uint64_t_values[26] = host_properties->oldest_key_time;
		table_uint64_t_values[27] = host_properties->file_number;
		table_uint64_t_values[30] = properties->raw_key_size;
		table_uint64_t_values[31] = properties->raw_value_size;
		table_uint64_t_values[32] = properties->tail_start_offset;
	}

	host_properties->file_number++;
	host_properties->file_creation_time++;

	// index_type is the only uint32_t that is encoded. treat it differently.
	shared = 0;
	non_shared = strlen(table_properties[0]);
	value_size = 4;
	properties_buf = EncodeVarint32(properties_buf, shared);
	properties_buf = EncodeVarint32(properties_buf, non_shared);
	properties_buf = EncodeVarint32(properties_buf, value_size);
	memcpy(properties_buf, table_properties[0] + shared, non_shared);
	properties_buf += non_shared;
	memcpy(properties_buf, table_char_values[0], 4);
	properties_buf += 4;
	// printk("%d: %s - (%d, %d, %d), %lu\n", 0, table_properties[0], shared, non_shared, value_size, (properties_buf - start_buf));
	last_key = (char *)table_properties[0];

	for (int i = 1; i < num_properties; i++) {
		shared = difference_offset(last_key, (char *)table_properties[i]);
		non_shared = strlen(table_properties[i]) - shared;

		if (strlen(table_char_values[i]) == 0) {
			tmp_buf = encoding_buf;
			tmp_buf = PutVarint64(tmp_buf, table_uint64_t_values[i]);
			value_size = (tmp_buf - encoding_buf);
		} else {
			value_size = strlen(table_char_values[i]);
		}

		properties_buf = EncodeVarint32(properties_buf, shared);
		properties_buf = EncodeVarint32(properties_buf, non_shared);
		properties_buf = EncodeVarint32(properties_buf, value_size);

		memcpy(properties_buf, table_properties[i] + shared, non_shared);
		properties_buf += non_shared;

		if (strlen(table_char_values[i]) == 0) {
			memcpy(properties_buf, encoding_buf, value_size);
			properties_buf += value_size;
		} else {
			memcpy(properties_buf, table_char_values[i], value_size);
			properties_buf += value_size;
		}

		// printk("%d: %s - (%d, %d, %d), %lu\n", i, table_properties[i], shared, non_shared, value_size, (properties_buf - start_buf));

		last_key = (char *)table_properties[i];
	}

	/* restarts_ and num_restarts */
	properties_buf = PutFixed32(properties_buf, (uint32_t)0);
	properties_buf = PutFixed32(properties_buf, (uint32_t)1);

	output_offset += (properties_buf - start_buf);

	return output_offset;
}

#endif