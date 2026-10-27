# 本地文件 SQL

`file-sql` 分支基于 `2026-competition` 的 `5bce9e06`。实现直接生成 seekdb 的原生表函数扫描计划；过滤、表达式、聚合、排序和 JOIN 由原有 SQL 算子执行。运行时不使用外部查询引擎，也不创建或导入中间数据表。

本功能已与 `ai-function-enhancement` 的 `70b33960` 整合验证。AI 生产实现沿用目标分支，双方的 C++ 测试使用共享主入口，原 Windows AI 测试适配器也通过该入口调用。

## 使用

首次启动时把专用文件目录设置为 `secure_file_priv`，例如 `seekdb --variable secure_file_priv=/absolute/allowed`。该设置仍受现有管理权限限制。`enable_file_sql` 默认关闭，每次绑定和执行文件查询都会重新校验开关与允许目录。

```sql
SET SESSION enable_file_sql = ON;

SELECT id, name, score
FROM read_csv('/absolute/allowed/basic.csv', header=true, sample_size=-1) AS f
WHERE score >= 90
ORDER BY score DESC LIMIT 10;

DESCRIBE SELECT * FROM read_csv('/absolute/allowed/basic.csv');
SELECT * FROM read_json('/absolute/allowed/basic.jsonl', format='newline_delimited');
SELECT * FROM read_ndjson('/absolute/allowed/basic.jsonl');
SELECT c1 FROM read_parquet('/absolute/allowed/simple.parquet');
SELECT file FROM glob('/absolute/allowed/*.csv') ORDER BY file;

SELECT a.id, b.name
FROM read_csv('/absolute/allowed/a.csv') a
LEFT JOIN read_json('/absolute/allowed/b.jsonl') b ON a.id=b.id;

COPY (SELECT id,name FROM read_csv('/absolute/allowed/basic.csv') ORDER BY id)
TO '/absolute/allowed/output.csv'
(FORMAT CSV, HEADER, DELIMITER ',', QUOTE '"', ESCAPE '"', NULLSTR 'NULL');

COPY ordinary_table TO '/absolute/allowed/table.csv' (HEADER);
```

文件路径必须是常量字符串。读取选项使用 `name=value`；COPY 选项支持 `name value` 或 `name=value`。CSV 默认有表头，分隔符为逗号，引号和转义字符均为双引号，NULL 文本为 `NULL`。`delim` 与 `delimiter` 等价，字符选项必须是单个字节。COPY 默认不写表头，使用 `HEADER` 或 `HEADER true` 开启；没有表头的导出应以 `read_csv(...,header=false)` 读回。

CSV/JSONL 的 `sample_size` 只接受 `-1`。两者先流式遍历全部记录以确定 Schema，再重新打开文件执行查询。未引用的空 CSV 字段与 NULL 文本解释为 NULL；带引号的空字符串及 NULL 文本保留为字符串。重复表头使用大小写不敏感的 `__2`、`__3` 后缀消歧。UTF-8、LF/CRLF、双引号转义、自定义转义和字段内换行均受支持。

类型集合为 BIGINT、DOUBLE、BOOLEAN、VARCHAR、DATE、DATETIME、NULL。整数与浮点数混合时宽化为 DOUBLE，DATE 与 DATETIME 混合时宽化为 DATETIME，其余冲突宽化为 VARCHAR。超出有符号 64 位范围的整数保留为 VARCHAR；日期校验闰年与日历边界；全 NULL 列保留 NULL 类型。JSONL 对象按字段首次出现顺序合并 Schema，缺失字段补 NULL；JSON 字符串保留字符串语义，合法日期/时间字符串可推断为日期类型。拒绝嵌套结构、重复或歧义字段及错误行，并返回位置诊断。

CSV/JSONL 的 DATETIME 小数精度取全列最大精度（0–6），并传递给原生 SQL 列元数据；整秒值不会额外显示 `.000000`，微秒值不会被截断。Parquet 的毫秒/微秒时间戳分别使用 3/6 位精度。

Parquet 从 Footer 获取扁平 Schema，按 Row Group 和 Page 读取，物理上只打开所引用列的 Column Chunk。支持基础类型、可空列、PLAIN、字典、RLE、Data Page V1/V2、UNCOMPRESSED/SNAPPY 和可选 Page CRC。支持日期与毫秒/微秒时间戳；UINT64 用 VARCHAR 保留完整数值。当前不支持嵌套/重复字段、DECIMAL、纳秒时间戳、其他压缩和编码。格式参考 [Apache Parquet Thrift 定义](https://github.com/apache/parquet-format/blob/master/src/main/thrift/parquet.thrift)及[编码规范](https://github.com/apache/parquet-format/blob/master/Encodings.md)。

`glob()` 发现单个目录中匹配 `*`/`?` 的文件，返回一列 `file`，不会自动合并文件。远程文件、分布式扫描及多文件自动合并不在本实现范围内。

## 安全与资源

输入只以只读方式打开。检查规范路径、目录边界、打开后的实际路径与文件身份；在读行、EOF 和重扫时检测大小、时间戳及文件替换。Windows 还拒绝 UNC、设备路径和 NTFS ADS。文件查询跳过计划缓存与字面量参数化，使下一条同文 SQL 重新绑定 Schema。

限制为 1,024 列、单条记录 16 MiB、Footer/Page 16 MiB、Parquet 驻留页与字典预算 64 MiB、glob 4,096 个文件；批量扫描按字节预算分批。数据不按全文件保存到内存。超限或解析失败以 SQL 错误返回。

COPY 在允许目录中独占创建临时文件；成功消费查询并完成 flush/sync 后才发布目标。已有目标不会被覆盖；查询失败、取消或未提交的 Writer 销毁时清理临时文件。Unix 使用目录句柄与 `openat/linkat`，Windows 在导出期间持有目录句柄，防止目录替换重定向写入。

Linux 发布目标前还会重新检查目录句柄在 `/proc/self/fd` 中的实际路径，拒绝导出期间将目录移出允许范围、再以新目录替换原路径的情况。

COPY 直接消费源 SELECT 的输出，保留 ORDER BY、重复列名与 UNION 结果。重复表头读回时按 CSV 规则消歧；源查询不能再带 INTO。

## 代码入口

- `src/sql/parser/sql_parser_mysql_mode.y`：表函数、DESCRIBE SELECT、COPY 语法。
- `src/sql/engine/basic/ob_file_sql_bridge.cpp`：命名参数、权限校验、绑定描述符。
- `src/sql/resolver/dml/ob_dml_resolver.cpp`、`ob_resolver_utils.cpp`：动态列及名称解析；`cmd/ob_show_resolver.cpp`：Schema 结果。
- `src/sql/code_generator/ob_static_engine_cg.cpp`、`engine/basic/ob_function_table_op.cpp`：原生计划、列编号映射、行/批量扫描和重扫。
- `src/sql/engine/basic/ob_file_scan_utils.cpp`、`ob_file_scan_parquet.inc`：流式解析、类型推断、文件身份与安全导出。
- `src/sql/engine/basic/ob_select_into_op.cpp`：COPY 驱动原生查询并处理成功/失败发布。
- `src/share/system_variable/ob_system_variable_init.json`：默认关闭的 `enable_file_sql` 定义。

## 复现测试

在已配置依赖的构建目录中启用专门测试目标：

```sh
cmake -S . -B build_release -DOB_BUILD_FILE_SQL_TESTS=ON
cmake --build build_release --target observer test_file_scan_utils test_file_sql_parser
ctest --test-dir build_release -R 'file_scan_utils|file_sql_parser|file_sql_shutdown_reporting' --output-on-failure
```

Windows 使用项目的 `build.ps1`/clang-cl 工具链与依赖路径；Unix 的模块测试入口为 `//unittest/sql:sql_tests`。Parquet 单元测试使用仓库内独立 PyArrow 生成的文件，运行单元测试不依赖 PyArrow。可用 `unittest/sql/engine/basic/generate_file_sql_fixtures.py` 重建这些兼容性样本。

原生 SQL 集成脚本需要 `pymysql`，应连接专用实例并使用专用允许目录。脚本会创建测试文件及 `file_sql_export_test` 表：

```sh
python -m pip install pymysql
mkdir -p /absolute/file-sql-data
seekdb --nodaemon --port 2883 --base-dir /absolute/test-instance \
  --data-dir /absolute/test-instance/store \
  --variable secure_file_priv=/absolute/file-sql-data \
  --parameter datafile_size=512M --parameter datafile_maxsize=2G \
  --parameter log_disk_size=2G

python unittest/sql/file_sql_integration.py --port 2883 \
  --root /absolute/file-sql-data --report /absolute/file-sql-results.json

python tools/module_check/sql_source_ownership_check.py
python tools/module_check/sql_header_ownership_check.py
python tools/module_check/unittest_module_check.py
```

2026-10-27 已在用户提供的 Ubuntu 22.04 x86_64 服务器完成 Linux 验收。容器配额为 12 核、42 GiB，使用项目依赖中的 Clang 17.0.6、Rust 1.98.1，以 RelWithDebInfo / `-O2 -g1 -DNDEBUG` 构建完整 seekdb。67 项原生 SQL 用例、21 项解析器用例、3,043 个扫描检查全部通过；新增停机报告测试后，CTest 为 3/3，包含 4 项真实子进程的信号/超时回归检查。三个工程归属检查通过。另以 GCC 11 的 AddressSanitizer、UndefinedBehaviorSanitizer 和泄漏检测执行扫描检查，全部通过；Windows Clang 20 的独立扫描检查为 3,042 项，通过。检查数的差异来自平台专属安全用例。

公开 `file_sql_basic.test` 的 SQL 与 `file_sql_basic.result` 保持原样，通过 Mysqltest 输出比较。为适配单机专用实例，只在运行目录替换了原先依赖 SSH/SCP 的数据部署和清理 include。新增 `file_sql_extended.test` 也执行通过；该扩展脚本没有预置结果快照，其错误码检查由 Mysqltest 执行，结果值由原生 SQL 集成脚本中的断言覆盖。覆盖跨格式 INNER/LEFT JOIN、相关重扫、Schema、CSV 导出读回/拒绝覆盖、COPY 排序与重复列名及 UNION、运行中修改输入、目录移出允许范围、导出中途失败、记录资源上限、损坏 Parquet CRC、未投影列裁剪及句柄释放。

服务器源码目录为 `/root/rivermind-data/seekdb-file-sql-run/source`，测试端口为 12883，实例已停止。日志和结果位于其父目录，本地副本位于 `build_release/linux-server-results/`。未运行 Linux Bazel 构建和官方隐藏评测，因此不据此声称官方得分。

## 正常停机与报告判定

项目现有正常停机信号为 `SIGUSR1`：`ob_signal_handle.cpp` 调用 `prepare_stop()`，固定预留 5 秒后调用 `set_stop()`；`ObServer::wait()` 每 3 秒检查停止标志并以状态 0 退出。典型耗时约 5–8 秒。对确认属于本次测试的进程使用 `kill -USR1 <PID>`。现有 `SIGTERM` 分支会主动发送 `SIGKILL`，产生 `-9` 退出码。

先前报告把 `-9` 固定描述为“等待 45 秒超时后强制结束”，该描述有误。原运行从启动到全部测试完成不足 9 秒；复现的空闲/完整负载实例收到 `SIGTERM` 后分别在 0.164/0.114 秒退出。新的停机验收使用 `SIGUSR1`，记录实际信号、耗时、退出码、是否捕获等待超时及是否由测试程序执行强杀；正常停机必须满足状态 0、无超时、无强杀。

`unittest/sql/file_sql_shutdown.py` 提供只操作自身 `Popen` 子进程的停机记录；`test_file_sql_shutdown.py` 用真实子进程覆盖正常退出、自行 SIGKILL、真实超时及提前失败四种情况。可运行 `python3 -B unittest/sql/test_file_sql_shutdown.py`，或在 Unix 上运行 CTest 的 `file_sql_shutdown_reporting`。

同台服务器复测：完整负载后正常退出 7.924 秒，重启后空闲退出 5.972 秒，保留空闲客户端时退出 5.922 秒。三次均为状态 0，无超时、无测试程序强杀；重启后的已提交表数据和文件查询均正确，端口释放，空闲客户端断开。该修复位于验收脚本与报告层，保留引擎原有的信号语义。

## 与 AI 函数共同验证

联合版本在同台 Linux 服务器构建并通过 CTest 4/4，包括 File SQL 扫描、解析、停机报告及 AI Prompt/Provider C++ 工具测试。另通过 14 项 AI SQL 回归（含公开 AI 测试脚本的 153 条语句）、67 项 File SQL 回归、两份 File SQL Mysqltest 脚本与 4 项联合场景；专用实例以状态 0 正常退出。

```sh
cmake -S . -B build_release -DOB_BUILD_FILE_SQL_TESTS=ON -DOB_BUILD_AI_FUNCTION_TESTS=ON
cmake --build build_release --target observer test_file_scan_utils test_file_sql_parser test_ai_function_utils
ctest --test-dir build_release -R 'file_scan_utils|file_sql_parser|file_sql_shutdown_reporting|ai_function_utils' --output-on-failure

python3 -B unittest/sql/file_sql_ai_integration.py --port 12883 \
  --root /absolute/allowed --report /absolute/combined-results.json
```

联合场景覆盖文件列进入 AI_PROMPT、AI_EMBED 的行/NULL 映射，以及 COPY 导出 AI 表达式后读回。AI_PROMPT 接受字符串或 JSON 参数并返回提示词 JSON 对象；数字列应显式 `CAST(... AS CHAR)`，读回断言比较 JSON 内容。HTTP 服务仅为本地确定性测试夹具，不访问真实模型。
