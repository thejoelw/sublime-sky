# automatically generated by the FlatBuffers compiler, do not modify

# namespace: SsProtocol

import flatbuffers
from flatbuffers.compat import import_numpy
np = import_numpy()

class InitRequest(object):
    __slots__ = ['_tab']

    @classmethod
    def GetRootAsInitRequest(cls, buf, offset):
        n = flatbuffers.encode.Get(flatbuffers.packer.uoffset, buf, offset)
        x = InitRequest()
        x.Init(buf, n + offset)
        return x

    # InitRequest
    def Init(self, buf, pos):
        self._tab = flatbuffers.table.Table(buf, pos)

def InitRequestStart(builder): builder.StartObject(0)
def InitRequestEnd(builder): return builder.EndObject()