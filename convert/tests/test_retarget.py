"""retarget.py on hand-made serialized files: a data file of one patch release made for the player of
another (version string in the metadata, object data kept; changed class layouts converted).
python -m unittest tests.test_retarget   (from convert/)"""
import os
import struct
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
from pmunity import retarget as R


def text_asset(name, text):
    def s(b):
        return struct.pack('<i', len(b)) + b + b'\0' * (-len(b) % 4)
    return s(name.encode()) + s(text.encode())


def serialized_file(version, objects, data_offset=None, platform=2):
    """A format 17 file without type trees: TextAssets (class 49) [(path id, bytes)], the object data
    8-aligned from data_offset (default: right after the metadata, 16-aligned; 'tight': right after it)."""
    meta = version.encode() + b'\0' + struct.pack('<IB', platform, 0)
    meta += struct.pack('<i', 1) + struct.pack('<iBh', 49, 0, -1) + b'\x11' * 16     # one type, its old hash
    meta += struct.pack('<i', len(objects))
    data, starts = b'', []
    for pid, blob in objects:
        starts.append(len(data))
        data += blob + b'\0' * (-len(blob) % 8)
    for (pid, blob), start in zip(objects, starts):
        meta += b'\0' * (-(20 + len(meta)) % 4)
        meta += struct.pack('<qIIi', pid, start, len(blob), 0)
    meta += struct.pack('<ii', 0, 0) + b'\0'           # no script types, no externals, user information ''
    end = 20 + len(meta)
    doff = end if data_offset == 'tight' else data_offset if data_offset is not None else end + (-end % 16)
    assert doff >= end
    head = struct.pack('>IIII', len(meta), doff + len(data), 17, doff) + b'\0' * 4
    return head + meta + b'\0' * (doff - end) + data


class RetargetTest(unittest.TestCase):
    OBJS = [(1, text_asset('a', 'hello')), (7, text_asset('second', 'x' * 37)), (9, text_asset('', ''))]

    def check(self, blob, version, moved):
        import UnityPy
        with tempfile.TemporaryDirectory() as d:
            p = os.path.join(d, 'level0')
            with open(p, 'wb') as f:
                f.write(blob)
            sf = R.parse_metadata(p)
            self.assertEqual(R.metadata_bytes(sf, sf.unity_version, 2), blob[20:20 + struct.unpack_from('>I', blob)[0]])
            note = R.retarget_file(p, '2017.4.9f1', version, 13)
            self.assertEqual(('moved' in note), moved, note)
            with open(p, 'rb') as f:
                new = f.read()
            doff0, doff1 = struct.unpack_from('>I', blob, 12)[0], struct.unpack_from('>I', new, 12)[0]
            self.assertEqual(new[doff1:], blob[doff0:])                     # the object data, unchanged
            self.assertEqual(struct.unpack_from('>I', new, 4)[0], len(new))
            self.assertEqual(doff1 % 16, doff0 % 16)
            sf = UnityPy.load(p).file
            self.assertEqual((sf.unity_version, sf._m_target_platform), (version, 13))
            self.assertEqual({k: o.get_raw_data() for k, o in sf.objects.items()}, dict(self.OBJS))

    def test_in_place(self):
        self.check(serialized_file('2017.4.9f1', self.OBJS, data_offset=4096), '2017.4.40f1', moved=False)

    def test_data_moved(self):
        # no room between metadata and data: a version string 4 bytes longer (the alignment before
        # the object entries takes at most 3 of them) moves the data by 16 bytes
        self.check(serialized_file('5.6.1f1', self.OBJS, data_offset='tight'), '2017.4.40f1', moved=True)

    def test_shorter_version(self):
        self.check(serialized_file('2017.4.9f1', self.OBJS), '2017.4.8f1', moved=False)

    def test_layout_changes(self):
        ch = R.changed_classes('2017.4.9f1', '2017.4.40f1')
        self.assertIn(129, ch)                  # PlayerSettings
        self.assertNotIn(1, ch)                 # GameObject
        self.assertIsNone(R.layout_change(129, '2017.4.9f1', '2017.4.9f1'))

    def test_convert_player_settings(self):
        from UnityPy.helpers import TypeTreeHelper
        from UnityPy.streams import EndianBinaryReader, EndianBinaryWriter
        a, b = R.layout_change(129, '2017.4.9f1', '2017.4.40f1')
        old = R.default(a)
        old['iosAppInBackgroundBehavior'] = 1
        old['displayResolutionDialog'] = 2
        old['productName'] = 'Game'
        notes = []
        new = R.convert(old, a, b, 'PlayerSettings', notes)
        self.assertNotIn('iosAppInBackgroundBehavior', new)
        self.assertIs(new['iosUseCustomAppBackgroundBehavior'], False)
        self.assertEqual((new['displayResolutionDialog'], new['productName']), (2, 'Game'))
        self.assertEqual(len(notes), 2, notes)
        w = EndianBinaryWriter(endian='<')
        TypeTreeHelper.write_typetree(new, b, w)
        data = w.bytes
        back = TypeTreeHelper.read_typetree(b, EndianBinaryReader(data, endian='<'), byte_size=len(data))
        self.assertEqual(back['productName'], 'Game')


if __name__ == '__main__':
    unittest.main()
