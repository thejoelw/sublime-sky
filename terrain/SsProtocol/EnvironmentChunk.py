# automatically generated by the FlatBuffers compiler, do not modify

# namespace: SsProtocol

import flatbuffers
from flatbuffers.compat import import_numpy
np = import_numpy()

class EnvironmentChunk(object):
    __slots__ = ['_tab']

    @classmethod
    def GetRootAsEnvironmentChunk(cls, buf, offset):
        n = flatbuffers.encode.Get(flatbuffers.packer.uoffset, buf, offset)
        x = EnvironmentChunk()
        x.Init(buf, n + offset)
        return x

    # EnvironmentChunk
    def Init(self, buf, pos):
        self._tab = flatbuffers.table.Table(buf, pos)

    # EnvironmentChunk
    def CellSizeLog2(self):
        o = flatbuffers.number_types.UOffsetTFlags.py_type(self._tab.Offset(4))
        if o != 0:
            return self._tab.Get(flatbuffers.number_types.Uint8Flags, o + self._tab.Pos)
        return 0

    # EnvironmentChunk
    def CellCoord(self):
        o = flatbuffers.number_types.UOffsetTFlags.py_type(self._tab.Offset(6))
        if o != 0:
            x = o + self._tab.Pos
            from SsProtocol.Vec3_u32 import Vec3_u32
            obj = Vec3_u32()
            obj.Init(self._tab.Bytes, x)
            return obj
        return None

    # EnvironmentChunk
    def CellPositions(self, j):
        o = flatbuffers.number_types.UOffsetTFlags.py_type(self._tab.Offset(8))
        if o != 0:
            x = self._tab.Vector(o)
            x += flatbuffers.number_types.UOffsetTFlags.py_type(j) * 12
            from SsProtocol.Vec3_f import Vec3_f
            obj = Vec3_f()
            obj.Init(self._tab.Bytes, x)
            return obj
        return None

    # EnvironmentChunk
    def CellPositionsLength(self):
        o = flatbuffers.number_types.UOffsetTFlags.py_type(self._tab.Offset(8))
        if o != 0:
            return self._tab.VectorLen(o)
        return 0

    # EnvironmentChunk
    def CellPositionsIsNone(self):
        o = flatbuffers.number_types.UOffsetTFlags.py_type(self._tab.Offset(8))
        return o == 0

    # EnvironmentChunk
    def CellEnvironments(self, j):
        o = flatbuffers.number_types.UOffsetTFlags.py_type(self._tab.Offset(10))
        if o != 0:
            x = self._tab.Vector(o)
            x += flatbuffers.number_types.UOffsetTFlags.py_type(j) * 4
            x = self._tab.Indirect(x)
            from SsProtocol.Environment import Environment
            obj = Environment()
            obj.Init(self._tab.Bytes, x)
            return obj
        return None

    # EnvironmentChunk
    def CellEnvironmentsLength(self):
        o = flatbuffers.number_types.UOffsetTFlags.py_type(self._tab.Offset(10))
        if o != 0:
            return self._tab.VectorLen(o)
        return 0

    # EnvironmentChunk
    def CellEnvironmentsIsNone(self):
        o = flatbuffers.number_types.UOffsetTFlags.py_type(self._tab.Offset(10))
        return o == 0

def EnvironmentChunkStart(builder): builder.StartObject(4)
def EnvironmentChunkAddCellSizeLog2(builder, cellSizeLog2): builder.PrependUint8Slot(0, cellSizeLog2, 0)
def EnvironmentChunkAddCellCoord(builder, cellCoord): builder.PrependStructSlot(1, flatbuffers.number_types.UOffsetTFlags.py_type(cellCoord), 0)
def EnvironmentChunkAddCellPositions(builder, cellPositions): builder.PrependUOffsetTRelativeSlot(2, flatbuffers.number_types.UOffsetTFlags.py_type(cellPositions), 0)
def EnvironmentChunkStartCellPositionsVector(builder, numElems): return builder.StartVector(12, numElems, 4)
def EnvironmentChunkAddCellEnvironments(builder, cellEnvironments): builder.PrependUOffsetTRelativeSlot(3, flatbuffers.number_types.UOffsetTFlags.py_type(cellEnvironments), 0)
def EnvironmentChunkStartCellEnvironmentsVector(builder, numElems): return builder.StartVector(4, numElems, 4)
def EnvironmentChunkEnd(builder): return builder.EndObject()
