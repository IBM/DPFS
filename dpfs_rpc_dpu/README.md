# dpfs_rpc_dpu
This binary consumes the dpfs_hal virtio-fs abstraction layer and sends requests directly over the wire to a remote server
using eRPC with the RVFS binary format.

# RVFS binary format
This format will describe the Remote Virtual File System protocol, i.e. virtio-fs directly over the wire via RPC.

The format is subject to change and work in progress. This protocol is not directly exposed to a consumer/cloud tenant,
the consumer/cloud tenant consumes virtio-fs. Therefore we do not build in any backwards compatibility into the wire format.

### Format
| Bytes | Data type | Name | Description |
| --- | --- | --- | -- |
| 0..4 | int32 | num_descs | The amount of descriptors in this message |
| 5..12 | uint64 | desc_len | The number of bytes in the descriptor following this uint64 |
| 13.. | raw data | desc_data | Descriptor data bytes |

Where multiple descriptors will follow each other in a single message.
## Example read request (Bytes are off!)
See linux/fuse.h for the FUSE struct definitions

| Bytes | Data type | Name | Contents |
| --- | --- | --- | -- |
| 0..4 | int32 | num_descs | 3 |
| 5..12 | uint64 | desc_len | `sizeof(fuse_in_header)` |
| 13..44 | raw data | desc_data | FUSE input header `fuse_in_header` |
| 45..52 | uint64 | desc_len | `sizeof(fuse_read_in)` |
| 53..84 | raw data | desc_data | FUSE read input header (`fuse_read_in`) |

## Example read reply (Bytes are off!)
See linux/fuse.h for the FUSE struct definitions

| Bytes | Data type | Name | Contents |
| --- | --- | --- | -- |
| 0..4 | int32 | num_descs | 3 |
| 5..12 | uint64 | desc_len | `sizeof(fuse_out_header)` |
| 13..44 | raw data | desc_data | FUSE out header `fuse_out_header` |
| 45..52 | uint64 | desc_len | Number of bytes read (here 32) |
| 53..84 | raw data | desc_data | Data |


