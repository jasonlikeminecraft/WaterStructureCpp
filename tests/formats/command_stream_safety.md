# Command-format stream safety cases

These cases are intentionally small and are consumed by the format test
runner (or can be exercised with `water_structure_cli inspect`).  They cover
the bounded-reader paths added for BDX, IBImport, MCFunction, KBDX and TIBI.

| case | input condition | expected result |
| --- | --- | --- |
| `mcfunction_truncated_line` | `fill 0 0 0 1 1` without a Z/name | parse error containing `命令参数不完整` and the source line number |
| `mcfunction_bad_state` | unterminated quoted state property | parse error containing `状态格式无效` |
| `mcfunction_command_limit` | more than 8,000,000 recognized commands | bounded failure containing `命令数量超过限制` |
| `ibimport_bad_varint` | continuation varint with a non-zero 10th-byte payload | bounded failure containing `varint` and `溢出` |
| `ibimport_segment_truncated` | segment length larger than remaining file bytes | failure containing `段长度超过文件剩余数据` |
| `ibimport_bad_coordinate` | recognized command with a non-decimal coordinate | failure containing `坐标无效` and the line number |
| `kbdx_record_truncated` | block count whose 20-byte records exceed file length | failure containing `方块记录截断` and the expected offset |
| `kbdx_deep_json` | metadata nesting deeper than 128 levels | failure containing `JSON nesting exceeds limit` |
| `tibi_payload_limit` | DEFLATE expands beyond the configured limit | failure containing `decoded payload exceeds` |
| `bdx_cursor_overflow` | cursor movement crosses the signed 32-bit range | failure containing `游标超出 int32` plus command/decoded offset |

The test process should run each case in a separate child process when a hard
memory limit is enabled.  This prevents a deliberately large malformed input
from affecting subsequent cases.
