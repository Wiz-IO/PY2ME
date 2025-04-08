#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#define MAX_STACK_SIZE 256
#define MAX_LOCALS 64
#define MAX_GLOBALS 32
#define MAX_OBJECTS 32
#define MAX_ATTRS_PER_OBJECT 8
#define MAX_NAME_LENGTH 32
#define MAX_MODULES 16

typedef struct {
    uint32_t object_count;
    uint32_t module_count;
    uint32_t text_size;
    uint32_t data_size;
    uint32_t rodata_size;
    uint32_t symtab_size;
    uint32_t entry_point_idx;  // Ново поле за entry point
} BinHeader;

typedef struct {
    uint32_t type;
    uint32_t section;
    uint32_t offset;
    uint32_t module_id;
} ObjectEntry;

typedef struct {
    BinHeader header;
    ObjectEntry* objects;
    uint8_t* text;
    uint8_t* data;
    uint8_t* rodata;
    uint8_t* symtab;
    uint8_t* modtab;
} BinFile;

typedef union {
    int32_t i;       // int, bool
    float f;         // float
    uint32_t obj_idx;// Указател към обект (instance, function, class, builtin, etc.)
    uint8_t* ptr;    // Указател към данни (string, bytes, list, tuple, dict)
} Value;

typedef struct {
    uint8_t name[MAX_NAME_LENGTH];
    Value value;
} Attribute;

typedef struct {
    Attribute attrs[MAX_ATTRS_PER_OBJECT];
    uint8_t attr_count;
    uint32_t class_idx;  // Индекс на класа (0 за класове)
} Object;

typedef struct {
    uint8_t name[MAX_NAME_LENGTH];
    Value value;
    uint32_t module_id;
} GlobalVar;

typedef struct {
    GlobalVar globals[MAX_GLOBALS];
    uint32_t global_count;
    uint32_t module_id;
} Namespace;

typedef struct {
    Value stack[MAX_STACK_SIZE];
    uint32_t stack_top;
    Value locals[MAX_LOCALS];
    Namespace namespaces[MAX_MODULES];
    Object objects[MAX_OBJECTS];
    uint32_t object_count;
    BinFile* bin;
} VM;

typedef Value (*BuiltinFunc)(VM*, Value*, uint16_t);
Value builtin_print(VM* vm, Value* args, uint16_t argc) {
    if (argc > 0) {
        if (args[0].obj_idx >= vm->bin->header.object_count) {
            printf("Invalid object index\n");
            return (Value){0};
        }
        ObjectEntry* entry = &vm->bin->objects[args[0].obj_idx];
        if (entry->type == 2) printf("%d\n", *(int32_t*)(vm->bin->data + entry->offset));
        else if (entry->type == 3) printf("%f\n", *(float*)(vm->bin->data + entry->offset));
        else if (entry->type == 4) {
            uint16_t len = *(uint16_t*)(vm->bin->data + entry->offset);
            printf("%.*s\n", len, (char*)(vm->bin->data + entry->offset + 2));
        }
        else if (entry->type == 5) {
            uint16_t len = *(uint16_t*)(vm->bin->rodata + entry->offset);
            printf("<bytes: %d bytes>\n", len);
        }
    }
    return (Value){0};
}
BuiltinFunc builtins[] = {builtin_print, NULL, NULL};  // print=0, len=1, range=2

void init_bin_file(BinFile* bin, uint8_t* data) {
    bin->header = *(BinHeader*)data;
    bin->objects = (ObjectEntry*)(data + 28);  // 7 * 4 байта за хедъра
    uint32_t table_size = bin->header.object_count * sizeof(ObjectEntry);
    bin->text = data + 28 + table_size;
    bin->data = bin->text + bin->header.text_size;
    bin->rodata = bin->data + bin->header.data_size;
    bin->symtab = bin->rodata + bin->header.rodata_size;
    bin->modtab = bin->symtab + (bin->header.symtab_size - bin->header.module_count * (2 + MAX_NAME_LENGTH));
}

void vm_init(VM* vm, BinFile* bin) {
    vm->stack_top = 0;
    vm->object_count = 0;
    vm->bin = bin;
    memset(vm->locals, 0, sizeof(vm->locals));
    memset(vm->namespaces, 0, sizeof(vm->namespaces));
    memset(vm->objects, 0, sizeof(vm->objects));
}

uint32_t vm_create_object(VM* vm, uint32_t class_idx) {
    if (vm->object_count >= MAX_OBJECTS) return 0;
    uint32_t idx = vm->object_count++;
    vm->objects[idx].class_idx = class_idx;
    return idx;
}

void vm_set_attr(VM* vm, uint32_t obj_idx, const uint8_t* name, uint16_t name_len, Value value) {
    Object* obj = &vm->objects[obj_idx];
    for (uint8_t i = 0; i < obj->attr_count; i++) {
        if (strncmp((char*)obj->attrs[i].name, (char*)name, name_len) == 0) {
            obj->attrs[i].value = value;
            return;
        }
    }
    if (obj->attr_count < MAX_ATTRS_PER_OBJECT) {
        strncpy((char*)obj->attrs[obj->attr_count].name, (char*)name, name_len);
        obj->attrs[obj->attr_count].value = value;
        obj->attr_count++;
    }
}

Value vm_get_attr(VM* vm, uint32_t obj_idx, const uint8_t* name, uint16_t name_len) {
    Object* obj = &vm->objects[obj_idx];
    for (uint8_t i = 0; i < obj->attr_count; i++) {
        if (strncmp((char*)obj->attrs[i].name, (char*)name, name_len) == 0) {
            return obj->attrs[i].value;
        }
    }
    if (obj->class_idx != 0) {
        return vm_get_attr(vm, obj->class_idx, name, name_len);
    }
    return (Value){0};
}

void vm_set_global(VM* vm, uint32_t module_id, const uint8_t* name, uint16_t name_len, Value value) {
    Namespace* ns = &vm->namespaces[module_id];
    for (uint32_t i = 0; i < ns->global_count; i++) {
        if (strncmp((char*)ns->globals[i].name, (char*)name, name_len) == 0) {
            ns->globals[i].value = value;
            return;
        }
    }
    if (ns->global_count < MAX_GLOBALS) {
        strncpy((char*)ns->globals[ns->global_count].name, (char*)name, name_len);
        ns->globals[ns->global_count].value = value;
        ns->globals[ns->global_count].module_id = module_id;
        ns->global_count++;
    }
}

Value vm_get_global(VM* vm, uint32_t module_id, const uint8_t* name, uint16_t name_len) {
    Namespace* ns = &vm->namespaces[module_id];
    for (uint32_t i = 0; i < ns->global_count; i++) {
        if (strncmp((char*)ns->globals[i].name, (char*)name, name_len) == 0) {
            return ns->globals[i].value;
        }
    }
    return (Value){0};
}

Value get_value_from_entry(VM* vm, uint32_t obj_idx) {
    ObjectEntry* entry = &vm->bin->objects[obj_idx];
    Value val = {0};
    switch (entry->type) {
        case 0:  // instance
            val.obj_idx = vm_create_object(vm, 0);  // Създава празна инстанция
            break;
        case 1:  // bytecode
        case 11: // function
            val.obj_idx = obj_idx;  // Указва към байткода
            break;
        case 2:  // int
            val.i = *(int32_t*)(vm->bin->data + entry->offset);
            break;
        case 3:  // float
            val.f = *(float*)(vm->bin->data + entry->offset);
            break;
        case 4:  // string
            val.ptr = vm->bin->data + entry->offset;
            break;
        case 5:  // bytes
            val.ptr = vm->bin->rodata + entry->offset;
            break;
        case 6:  // list
        case 7:  // tuple
            val.ptr = vm->bin->rodata + entry->offset;
            break;
        case 8:  // dict
            val.ptr = vm->bin->rodata + entry->offset;
            break;
        case 9:  // bool
            val.i = *(uint8_t*)(vm->bin->data + entry->offset);
            break;
        case 10: // none
            val.i = 0;
            break;
        case 14: // class
            val.obj_idx = vm_create_object(vm, 0);  // Създава клас обект
            break;
        case 16: // builtin
            val.obj_idx = *(uint32_t*)(vm->bin->data + entry->offset);
            break;
        default:
            printf("Unsupported type: %d\n", entry->type);
            break;
    }
    return val;
}

void vm_run(VM* vm, uint8_t* bytecode, uint32_t size, uint32_t module_id) {
    uint32_t pc = 0;
    while (pc < size) {
        uint8_t opcode = bytecode[pc++];
        uint16_t arg = bytecode[pc++] | (bytecode[pc++] << 8);
        switch (opcode) {
            case 1: {  // LOAD_CONST
                vm->stack[vm->stack_top++] = get_value_from_entry(vm, arg);
                break;
            }
            case 100: {  // STORE_FAST
                vm->locals[arg] = vm->stack[--vm->stack_top];
                break;
            }
            case 101: {  // LOAD_FAST
                vm->stack[vm->stack_top++] = vm->locals[arg];
                break;
            }
            case 23: {  // BINARY_ADD
                Value b = vm->stack[--vm->stack_top];
                Value a = vm->stack[--vm->stack_top];
                if (vm->bin->objects[a.obj_idx].type == 2 && vm->bin->objects[b.obj_idx].type == 2) {
                    vm->stack[vm->stack_top++].i = a.i + b.i;
                } else {
                    printf("Unsupported addition\n");
                }
                break;
            }
            case 71: {  // LOAD_BUILD_CLASS
                uint32_t class_idx = vm_create_object(vm, 0);
                vm->stack[vm->stack_top++].obj_idx = class_idx;
                break;
            }
            case 90: {  // STORE_NAME
                ObjectEntry* entry = &vm->bin->objects[arg];
                uint16_t name_len = *(uint16_t*)(vm->bin->symtab + entry->offset);
                Value value = vm->stack[--vm->stack_top];
                vm_set_global(vm, module_id, vm->bin->symtab + entry->offset + 2, name_len, value);
                break;
            }
            case 101: {  // LOAD_NAME
                ObjectEntry* entry = &vm->bin->objects[arg];
                if (entry->type == 16) {  // builtin
                    vm->stack[vm->stack_top++].obj_idx = *(uint32_t*)(vm->bin->data + entry->offset);
                } else {
                    uint16_t name_len = *(uint16_t*)(vm->bin->symtab + entry->offset);
                    vm->stack[vm->stack_top++] = vm_get_global(vm, module_id, vm->bin->symtab + entry->offset + 2, name_len);
                }
                break;
            }
            case 95: {  // STORE_ATTR
                ObjectEntry* entry = &vm->bin->objects[arg];
                uint16_t name_len = *(uint16_t*)(vm->bin->symtab + entry->offset);
                Value value = vm->stack[--vm->stack_top];
                Value obj = vm->stack[--vm->stack_top];
                vm_set_attr(vm, obj.obj_idx, vm->bin->symtab + entry->offset + 2, name_len, value);
                break;
            }
            case 106: {  // LOAD_ATTR
                ObjectEntry* entry = &vm->bin->objects[arg];
                uint16_t name_len = *(uint16_t*)(vm->bin->symtab + entry->offset);
                Value obj = vm->stack[--vm->stack_top];
                vm->stack[vm->stack_top++] = vm_get_attr(vm, obj.obj_idx, vm->bin->symtab + entry->offset + 2, name_len);
                break;
            }
            case 131: {  // CALL_FUNCTION
                uint16_t argc = arg;
                Value func = vm->stack[--vm->stack_top];
                Value args[argc];
                for (int i = argc - 1; i >= 0; i--) {
                    args[i] = vm->stack[--vm->stack_top];
                }
                if (func.obj_idx < sizeof(builtins) / sizeof(builtins[0]) && builtins[func.obj_idx]) {
                    vm->stack[vm->stack_top++] = builtins[func.obj_idx](vm, args, argc);
                } else {
                    ObjectEntry* entry = &vm->bin->objects[func.obj_idx];
                    if (entry->type == 1 || entry->type == 11) {  // bytecode or function
                        uint32_t offset = entry->offset;
                        for (int i = 0; i < argc; i++) {
                            vm->locals[i] = args[i];
                        }
                        vm_run(vm, vm->bin->text + offset, vm->bin->header.text_size - offset, entry->module_id);
                        if (vm->stack_top > 0) {
                            Value result = vm->stack[--vm->stack_top];
                            vm->stack[vm->stack_top++] = result;
                        }
                    }
                }
                break;
            }
            case 83:  // RETURN_VALUE
                return;
            default:
                printf("Unknown opcode: %d\n", opcode);
                return;
        }
    }
}

int main() {
    extern uint8_t bin_data[];  // Предполагаме, че BIN е вграден
    BinFile bin;
    init_bin_file(&bin, bin_data);

    VM vm;
    vm_init(&vm, &bin);

    if (bin.header.entry_point_idx != 0xFFFFFFFF) {
        ObjectEntry* entry = &bin.objects[bin.header.entry_point_idx];
        if (entry->type == 1 || entry->type == 11) {  // bytecode или function
            vm_run(&vm, bin.text + entry->offset, bin.header.text_size - entry->offset, entry->module_id);
        } else {
            printf("Invalid entry point type: %d\n", entry->type);
        }
    } else {
        printf("No entry point defined\n");
        // По подразбиране изпълняваме първия модул с module_id=0 (за съвместимост)
        for (uint32_t i = 0; i < bin.header.object_count; i++) {
            if (bin.objects[i].type == 1 && bin.objects[i].module_id == 0) {
                vm_run(&vm, bin.text + bin.objects[i].offset, bin.header.text_size, 0);
                break;
            }
        }
    }

    return 0;
}