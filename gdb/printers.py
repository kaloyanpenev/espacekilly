import gdb
import re

class PmrArrayPrinter:
    """Pretty-printer for pc::pmr_array<T, Capacity>."""

    def __init__(self, val):
        self.val = val
        self.buf = val['buf']

        # extract Capacity from the type name, e.g. "pc::pmr_array<Order, 128>"
        type_name = str(val.type.strip_typedefs())
        match = re.search(r',\s*(\d+)', type_name)
        self.capacity = int(match.group(1)) if match else 0

    def to_string(self):
        return f'pmr_array of size {self.capacity}'

    def children(self):
        for i in range(self.capacity):
            yield f'[{i}]', (self.buf + i).dereference()

    def display_hint(self):
        return 'array'


def pmr_array_lookup(val):
    type_name = str(val.type.strip_typedefs())
    if type_name.startswith('pc::pmr_array<'):
        return PmrArrayPrinter(val)
    return None


gdb.pretty_printers.append(pmr_array_lookup)
