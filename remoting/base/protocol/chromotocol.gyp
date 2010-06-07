# Copyright (c) 2010 The Chromium Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

{
  'variables': {
    'chromium_code': 1,
    'out_dir': '<(SHARED_INTERMEDIATE_DIR)/protoc_out/remoting/base/protocol',
  },
  'targets': [
    {
      # Protobuf compiler / generate rule for chromoting.proto.
      'target_name': 'chromotocol_proto',
      'type': 'none',
      'sources': [
        'chromotocol.proto',
      ],
      'rules': [
        {
          'rule_name': 'genproto',
          'extension': 'proto',
          'inputs': [
            '<(PRODUCT_DIR)/<(EXECUTABLE_PREFIX)protoc<(EXECUTABLE_SUFFIX)',
          ],
          'outputs': [
            '<(PRODUCT_DIR)/pyproto/chromotocol_pb/<(RULE_INPUT_ROOT)_pb2.py',
            '<(out_dir)/<(RULE_INPUT_ROOT).pb.cc',
            '<(out_dir)/<(RULE_INPUT_ROOT).pb.h',
          ],
          'action': [
            '<(PRODUCT_DIR)/<(EXECUTABLE_PREFIX)protoc<(EXECUTABLE_SUFFIX)',
            '--proto_path=.',
            './<(RULE_INPUT_ROOT)<(RULE_INPUT_EXT)',
            '--cpp_out=<(out_dir)',
            '--python_out=<(PRODUCT_DIR)/pyproto/chromotocol_pb',
          ],
          'message': 'Generating C++ and Python code from <(RULE_INPUT_PATH)',
        },
      ],
      'dependencies': [
        '../../../third_party/protobuf2/protobuf.gyp:protoc#host',
      ],
      # This target exports a hard dependency because it generates header
      # files.
      'hard_dependency': 1,
    },

    {
      'target_name': 'chromotocol_proto_lib',
      'type': '<(library)',
      'export_dependent_settings': [
        '../../../third_party/protobuf2/protobuf.gyp:protobuf_lite',
        'chromotocol_proto',
      ],
      'dependencies': [
        '../../../third_party/protobuf2/protobuf.gyp:protobuf_lite',
        'chromotocol_proto',
      ],
      # This target exports a hard dependency because depedents require
      # chromotocol_proto to compile.
      'hard_dependency': 1,
      'direct_dependent_settings': {
        'include_dirs': [
          '<(SHARED_INTERMEDIATE_DIR)/protoc_out',
        ],
      },
      'sources': [
        '<(out_dir)/chromotocol.pb.cc',
        '<(out_dir)/chromotocol.pb.h',
      ],
    },
  ],
}

# Local Variables:
# tab-width:2
# indent-tabs-mode:nil
# End:
# vim: set expandtab tabstop=2 shiftwidth=2:
