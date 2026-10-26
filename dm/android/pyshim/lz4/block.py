# lz4.block.decompress(data, uncompressed_size=n) through liblz4's LZ4_decompress_safe (ctypes); the only call the setup scripts make
import ctypes, os
_lib = ctypes.CDLL(os.path.join(os.environ.get('NX_NATIVE_LIB_DIR', ''), 'liblz4.so'))
_lib.LZ4_decompress_safe.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_int, ctypes.c_int]
def decompress(source, uncompressed_size=-1, return_bytearray=False, dict=None):
    if uncompressed_size < 0: raise ValueError('uncompressed_size is required')
    out = ctypes.create_string_buffer(uncompressed_size)
    n = _lib.LZ4_decompress_safe(bytes(source), out, len(source), uncompressed_size)
    if n < 0: raise ValueError('LZ4 block is corrupt')
    data = out.raw[:n]
    return bytearray(data) if return_bytearray else data
