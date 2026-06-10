#!/usr/bin/env python3
"""
Post-process expression_assets.bin to inject left/ GIF files.
The mmap_assets format:
  Header: files_count(4) + checksum(4) + data_len(4) = 12 bytes
  Per file: name(32) + size(4) + offset(4) + width(2) + height(2) = 44 bytes
  Data: 0x5A5A prefix(2) + file_data for each file
"""
import struct, os, sys

def compute_checksum(data):
    return sum(data) & 0xFFFF

def inject_gifs(bin_path, gif_dir, output_path, max_name_len=16):
    with open(bin_path, 'rb') as f:
        orig = f.read()

    # Parse header
    files_count = struct.unpack('<I', orig[0:4])[0]
    stored_chksum = struct.unpack('<I', orig[4:8])[0]
    data_len = struct.unpack('<I', orig[8:12])[0]

    # Parse file table
    FILE_ENTRY_SIZE = 44
    table_start = 12
    files = []
    for i in range(files_count):
        off = table_start + i * FILE_ENTRY_SIZE
        name = orig[off:off+32].split(b'\0')[0].decode('utf-8', errors='replace')
        size = struct.unpack('<I', orig[off+32:off+36])[0]
        data_offset = struct.unpack('<I', orig[off+36:off+40])[0]
        width = struct.unpack('<H', orig[off+40:off+42])[0]
        height = struct.unpack('<H', orig[off+42:off+44])[0]
        files.append({
            'name': name, 'size': size, 'data_offset': data_offset,
            'width': width, 'height': height
        })

    # Collect GIF files to add
    gif_files = sorted([f for f in os.listdir(gif_dir) if f.lower().endswith('.gif')])
    
    # Skip already existing files
    existing_names = {f['name'] for f in files}
    new_gifs = [g for g in gif_files if g not in existing_names]
    
    if not new_gifs:
        print("No new GIFs to add")
        shutil.copy2(bin_path, output_path)
        return

    print(f"Adding {len(new_gifs)} GIFs: {new_gifs}")

    # Read GIF data (after the file table and existing data)
    data_start = table_start + files_count * FILE_ENTRY_SIZE
    all_data = bytearray(orig[data_start:])

    for gif_name in new_gifs:
        gif_path = os.path.join(gif_dir, gif_name)
        with open(gif_path, 'rb') as f:
            gif_data = f.read()
        
        # Pad name to max_name_len
        name_bytes = gif_name.encode('utf-8')[:max_name_len]
        name_padded = name_bytes + b'\0' * (max_name_len - len(name_bytes))
        
        new_entry = {
            'name': gif_name,
            'size': len(gif_data),
            'data_offset': len(all_data),
            'width': 160,
            'height': 160
        }
        files.append(new_entry)
        
        # Add 0x5A5A prefix + file data
        all_data.extend(b'\x5A\x5A')
        all_data.extend(gif_data)
        print(f"  Added {gif_name}: {len(gif_data)} bytes")

    # Rebuild binary
    new_count = len(files)
    table_size = new_count * FILE_ENTRY_SIZE
    new_data_len = table_size + len(all_data)
    new_checksum = compute_checksum(all_data)

    output = bytearray()
    output.extend(struct.pack('<I', new_count))
    output.extend(struct.pack('<I', new_checksum))
    output.extend(struct.pack('<I', new_data_len))
    
    for f in files:
        name = f['name'].encode('utf-8')[:max_name_len]
        name_padded = name + b'\0' * (max_name_len - len(name))
        output.extend(name_padded)
        output.extend(struct.pack('<I', f['size']))
        output.extend(struct.pack('<I', f['data_offset']))
        output.extend(struct.pack('<H', f['width']))
        output.extend(struct.pack('<H', f['height']))
    
    output.extend(all_data)

    with open(output_path, 'wb') as f:
        f.write(output)
    
    print(f"Output: {output_path} ({len(output)} bytes, {new_count} files)")

if __name__ == '__main__':
    if len(sys.argv) < 4:
        print(f"Usage: {sys.argv[0]} <input.bin> <gif_dir> <output.bin>")
        sys.exit(1)
    inject_gifs(sys.argv[1], sys.argv[2], sys.argv[3])
