import marshal
import struct
import types
import os
import sys
import glob
import dis

DEFAULT_PYC_DIR = "__pycache__"
BIN_FILE = "bytecode.bin"

OBJECT_TYPES = {
    'instance': 0,
    'bytecode': 1,
    'int': 2,
    'float': 3,
    'string': 4,
    'bytes': 5,
    'list': 6,
    'tuple': 7,
    'dict': 8,
    'bool': 9,
    'none': 10,
    'function': 11,
    'name': 12,
    'varname': 13,
    'class': 14,
    'module': 15, 
    'builtin': 16 
}

class BinaryGenerator:
    def __init__(self):
        self.text_section = bytearray()
        self.data_section = bytearray()
        self.rodata_section = bytearray()
        self.symtab_section = bytearray()
        self.modtab_section = bytearray()
        self.objects_table = []
        self.symbols_map = {}
        self.constants_map = {}
        self.modules = {}
        self.module_count = 0
        self.builtins = {'print': 0, 'len': 1, 'range': 2}
        self.entry_point_idx = 0xFFFFFFFF

    def add_to_section(self, section, data, obj_type, section_name, offset_map=None, module_id=None):
        if offset_map is not None:
            key = str(data) + (f"_{module_id}" if module_id is not None else "")
            if key in offset_map:
                return offset_map[key]
            offset = len(section)
            idx = len(self.objects_table)
            offset_map[key] = idx
        else:
            offset = len(section)
            idx = len(self.objects_table)
        
        section.extend(data)
        self.objects_table.append((OBJECT_TYPES[obj_type], section_name, offset, module_id))
        return idx

    def process_bytecode(self, code_obj, module_id, is_function=False):
        idx = self.add_to_section(
            self.text_section, 
            code_obj.co_code, 
            'function' if is_function else 'bytecode', 
            'text', 
            module_id=module_id
        )
        return idx

    def process_constant(self, const, module_id):
        if const is None:
            return self.add_to_section(self.data_section, b'', 'none', 'data', self.constants_map, module_id)
        elif isinstance(const, types.CodeType):
            return None
        elif isinstance(const, int):
            data = struct.pack('<i', const)
            return self.add_to_section(self.data_section, data, 'int', 'data', self.constants_map, module_id)
        elif isinstance(const, float):
            data = struct.pack('<f', const)
            return self.add_to_section(self.data_section, data, 'float', 'data', self.constants_map, module_id)
        elif isinstance(const, str):
            ascii_str = const.encode('ascii', errors='ignore')
            data = struct.pack('<H', len(ascii_str)) + ascii_str
            return self.add_to_section(self.data_section, data, 'string', 'data', self.constants_map, module_id)
        elif isinstance(const, bytes):
            data = struct.pack('<H', len(const)) + const
            return self.add_to_section(self.rodata_section, data, 'bytes', 'rodata', self.constants_map, module_id)
        elif isinstance(const, bool):
            data = struct.pack('<B', 1 if const else 0)
            return self.add_to_section(self.data_section, data, 'bool', 'data', self.constants_map, module_id)
        elif isinstance(const, list):
            data = bytearray()
            data.extend(struct.pack('<H', len(const)))
            for item in const:
                idx = self.process_constant(item, module_id)
                data.extend(struct.pack('<I', idx))
            return self.add_to_section(self.rodata_section, data, 'list', 'rodata', self.constants_map, module_id)
        elif isinstance(const, tuple):
            data = bytearray()
            data.extend(struct.pack('<H', len(const)))
            for item in const:
                idx = self.process_constant(item, module_id)
                data.extend(struct.pack('<I', idx))
            return self.add_to_section(self.rodata_section, data, 'tuple', 'rodata', self.constants_map, module_id)
        elif isinstance(const, dict):
            data = bytearray()
            data.extend(struct.pack('<H', len(const)))
            for key, value in const.items():
                key_idx = self.process_constant(key, module_id)
                val_idx = self.process_constant(value, module_id)
                data.extend(struct.pack('<II', key_idx, val_idx))
            return self.add_to_section(self.rodata_section, data, 'dict', 'rodata', self.constants_map, module_id)

    def process_name(self, name, module_id):
        if name in self.builtins:
            data = struct.pack('<I', self.builtins[name])
            return self.add_to_section(self.data_section, data, 'builtin', 'data', self.symbols_map, module_id)
        ascii_name = name.encode('ascii', errors='ignore')
        data = struct.pack('<H', len(ascii_name)) + ascii_name
        return self.add_to_section(self.symtab_section, data, 'name', 'symtab', self.symbols_map, module_id)

    def process_varname(self, varname, module_id):
        ascii_varname = varname.encode('ascii', errors='ignore')
        data = struct.pack('<H', len(ascii_varname)) + ascii_varname
        return self.add_to_section(self.symtab_section, data, 'varname', 'symtab', self.symbols_map, module_id)

    def process_class(self, code_obj, module_id):
        instructions = list(dis.get_instructions(code_obj))
        class_name = None
        for instr in instructions:
            if instr.opname == 'STORE_NAME':
                class_name = instr.argval
                break
        if class_name:
            data = bytearray()
            name_idx = self.process_name(class_name, module_id)
            data.extend(struct.pack('<I', name_idx))
            method_count = 0
            for const in code_obj.co_consts:
                if isinstance(const, types.CodeType):
                    method_idx = self.process_bytecode(const, module_id, is_function=True)
                    data.extend(struct.pack('<I', method_idx))
                    method_count += 1
            data = struct.pack('<H', method_count) + data
            return self.add_to_section(self.rodata_section, data, 'class', 'rodata', self.constants_map, module_id)
        return None

    def find_entry_point(self, code_obj, module_id):
        instructions = list(dis.get_instructions(code_obj))
        for i, instr in enumerate(instructions):
            if (instr.opname == 'LOAD_NAME' and instr.argval == '__name__' and
                i + 1 < len(instructions) and instructions[i + 1].opname == 'LOAD_CONST' and
                instructions[i + 1].argval == '__main__' and
                i + 2 < len(instructions) and instructions[i + 2].opname == 'COMPARE_OP' and
                instructions[i + 2].arg == 2):
                return self.process_bytecode(code_obj, module_id)
        return None

    def process_code(self, code_obj, module_id):
        bytecode_idx = self.process_bytecode(code_obj, module_id)
        for const in code_obj.co_consts:
            self.process_constant(const, module_id)
        for name in code_obj.co_names:
            self.process_name(name, module_id)
        for varname in code_obj.co_varnames:
            self.process_varname(varname, module_id)
        for freevar in code_obj.co_freevars:
            self.process_varname(freevar, module_id)
        for cellvar in code_obj.co_cellvars:
            self.process_varname(cellvar, module_id)
        for const in code_obj.co_consts:
            if isinstance(const, types.CodeType):
                instructions = list(dis.get_instructions(code_obj))
                is_class = any(instr.opname == 'LOAD_BUILD_CLASS' for instr in instructions)
                if is_class:
                    self.process_class(const, module_id)
                else:
                    self.process_code(const, module_id)
        return bytecode_idx

    def process_module(self, pyc_path):
        module_name = os.path.basename(pyc_path).split('.')[0]
        if module_name not in self.modules:
            self.modules[module_name] = self.module_count
            self.module_count += 1
            name_data = module_name.encode('ascii', errors='ignore')
            self.add_to_section(self.modtab_section, struct.pack('<H', len(name_data)) + name_data, 'module', 'symtab')
        
        module_id = self.modules[module_name]
        code_obj = self.read_pyc(pyc_path)
        bytecode_idx = self.process_code(code_obj, module_id)
        
        if self.entry_point_idx == 0xFFFFFFFF:
            entry_idx = self.find_entry_point(code_obj, module_id)
            if entry_idx is not None:
                self.entry_point_idx = entry_idx

    def read_pyc(self, pyc_path):
        with open(pyc_path, 'rb') as f:
            f.read(16)
            return marshal.load(f)

    def write_bin(self, output_path):
        with open(output_path, 'wb') as f:
            header = struct.pack(
                '<IIIIIII',
                len(self.objects_table),
                self.module_count,
                len(self.text_section),
                len(self.data_section),
                len(self.rodata_section),
                len(self.symtab_section) + len(self.modtab_section),
                self.entry_point_idx
            )
            f.write(header)

            for type_id, section_name, offset, module_id in self.objects_table:
                section_id = {'text': 1, 'data': 2, 'rodata': 3, 'symtab': 4}[section_name]
                module_id = module_id if module_id is not None else 0xFFFFFFFF
                f.write(struct.pack('<IIII', type_id, section_id, offset, module_id))

            f.write(self.text_section)
            f.write(self.data_section)
            f.write(self.rodata_section)
            f.write(self.symtab_section)
            f.write(self.modtab_section)

    def generate_bin_from_directory(self, pyc_dir=DEFAULT_PYC_DIR, output_path=BIN_FILE):
        pyc_files = glob.glob(os.path.join(pyc_dir, "*.pyc"))
        if not pyc_files:
            print(f"Error: No .pyc files found in {pyc_dir}")
            return

        print(f"Found {len(pyc_files)} .pyc files: {pyc_files}")
        for pyc_path in pyc_files:
            print(f"Processing {pyc_path}...")
            self.process_module(pyc_path)

        self.write_bin(output_path)
        print(f"Generated BIN file: {output_path}")
        print(f"Entry point index: {self.entry_point_idx if self.entry_point_idx != 0xFFFFFFFF else 'None'}")
        print(f"Modules: {self.module_count}")
        print(f"Objects: {len(self.objects_table)}")

def main():
    pyc_dir = DEFAULT_PYC_DIR
    bin_file = BIN_FILE
    if len(sys.argv) > 1:
        pyc_dir = sys.argv[1]
    if len(sys.argv) > 2:
        bin_file = sys.argv[2]

    generator = BinaryGenerator()
    generator.generate_bin_from_directory(pyc_dir, bin_file)

if __name__ == "__main__":
    main()
