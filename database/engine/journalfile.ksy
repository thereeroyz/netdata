meta:
  id: netdata_journalfile_v2
  endian: le

seq:
  - id: journal_v2_header
    type: journal_v2_header
    size: 4096
  - id: extent_list
    type: journal_v2_extent_list
    repeat: expr
    repeat-expr: journal_v2_header.extent_count
  - id: extent_trailer
    type: journal_v2_block_trailer
  - id: metric_list
    type: journal_v2_metric_list
    repeat: expr
    repeat-expr: journal_v2_header.metric_count
  - id: metric_trailer
    type: journal_v2_block_trailer
  - id: page_blocs
    type: jounral_v2_page_blocs
    size: _root._io.size - _root._io.pos - 4
  - id: journal_file_trailer
    type: journal_v2_block_trailer


types:
  journal_v2_metric_list:
    seq:
      - id: uuid
        size: 16
      - id: entries
        type: u4
      - id: page_offset
        type: u4
      - id: delta_start_s
        type: u4
      - id: delta_end_s
        type: u4
    instances:
      page_block:
        type: journal_v2_page_block
        io: _root._io
        pos: page_offset
  journal_v2_page_hdr:
    seq:
      - id: crc
        type: u4
      - id: uuid_offset
        type: u4
      - id: entries
        type: u4
      - id: uuid
        size: 16
  journal_v2_page_list:
    seq:
      - id: delta_start_s
        type: u4
      - id: delta_end_s
        type: u4
      - id: extent_idx
        type: u4
      - id: update_every_s
        type: u4
      - id: page_len
        type: u2
      - id: type
        type: u1
      - id: reserved
        type: u1
    instances:
      extent:
        io: _root._io
        type: journal_v2_extent_list
        pos: _root.journal_v2_header.extent_offset + (extent_idx * 16)
  journal_v2_header:
    seq:
      - id: magic
        contents: [ 0x19, 0x10, 0x22, 0x01 ] #0x01221019
      - id: reserved
        type: u4
      - id: start_time_ut
        type: u8
      - id: end_time_ut
        type: u8
      - id: extent_count
        type: u4
      - id: extent_offset
        type: u4
      - id: metric_count
        type: u4
      - id: metric_offset
        type: u4
      - id: page_count
        type: u4
      - id: page_offset
        type: u4
      - id: extent_trailer_offset
        type: u4
      - id: metric_trailer_offset
        type: u4
      - id: original_file_size
        type: u4
      - id: total_file_size
        type: u4
      - id: data
        type: u8
    instances:
      trailer:
        io: _root._io
        type: journal_v2_block_trailer
        pos: _root._io.size - 4
  journal_v2_block_trailer:
    seq:
      - id: checksum
        type: u4
  journal_v2_extent_list:
    seq:
      - id: datafile_offset
        type: u8
      - id: datafile_size
        type: u4
      - id: file_idx
        type: u2
      - id: page_cnt
        type: u1
      - id: padding
        type: u1
  journal_v2_page_block:
    seq:
      - id: hdr
        type: journal_v2_page_hdr
      - id: page_list
        type: journal_v2_page_list
        repeat: expr
        repeat-expr: hdr.entries
      - id: block_trailer
        type: journal_v2_block_trailer
  jounral_v2_page_blocs:
    seq:
      - id: blocs
        type: journal_v2_page_block
        repeat: eos
