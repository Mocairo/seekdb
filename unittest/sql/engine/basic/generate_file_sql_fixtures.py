"""Generate independent Parquet compatibility fixtures; PyArrow is test-only."""
import datetime
import pathlib
import sys
import pyarrow as pa
import pyarrow.parquet as pq

root = pathlib.Path(sys.argv[1])
root.mkdir(parents=True, exist_ok=True)
table = pa.table({
    'id': pa.array(range(257), type=pa.int64()),
    'score': pa.array([i / 4 for i in range(257)], type=pa.float64()),
    'active': pa.array([bool(i % 2) for i in range(257)]),
    'day': pa.array([datetime.date(2024, 2, 29)] * 257, type=pa.date32()),
    'name': pa.array([None if i % 7 == 0 else f'value-{i % 3}' for i in range(257)]),
})
for name, dictionary, compression, version in [
    ('plain', False, 'NONE', '1.0'),
    ('dictionary', True, 'snappy', '1.0'),
    ('v2', True, 'snappy', '2.0'),
]:
    pq.write_table(table, root / f'{name}.parquet', row_group_size=97, data_page_size=128, write_batch_size=16,
                   use_dictionary=dictionary, compression=compression,
                   data_page_version=version, write_page_checksum=True)
pq.write_table(pa.table({'id': pa.array([], type=pa.int64()),
                        'name': pa.array([], type=pa.string())}), root / 'empty.parquet')
