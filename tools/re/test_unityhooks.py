"""unityhooks.py's checks of what the runtime's jump may replace, on hand-assembled instructions
(no Unity code): python3 -m unittest discover -s tools/re -p 'test_*.py'"""
import os
import struct
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import unityhooks  # noqa: E402


class Movable(unittest.TestCase):
    def test_arm(self):
        ok = [0xe92d4070,   # push {r4, r5, r6, lr}
              0xe1a04001,   # mov r4, r1
              0xe24dd008,   # sub sp, sp, #8
              0xe3500003]   # cmp r0, #3
        bad = [0xe59f0010,  # ldr r0, [pc, #16]
               0xe08f0000,  # add r0, pc, r0
               0x0a000004,  # beq
               0xeb000010,  # bl
               0xe12fff1e,  # bx lr
               0xfa000001,  # blx imm
               0xe8bd8070]  # pop {r4, r5, r6, pc}
        for w in ok:
            self.assertTrue(unityhooks.arm_movable(w), hex(w))
        for w in bad:
            self.assertFalse(unityhooks.arm_movable(w), hex(w))

    def test_a64(self):
        ok = [0xa9be57fe,   # stp x30, x21, [sp, #-0x20]!
              0xa9014ff4,   # stp x20, x19, [sp, #0x10]
              0x71000c1f,   # cmp w0, #3
              0x2a0103f3]   # mov w19, w1
        bad = [0xf0ff9409,  # adrp x9, ...
               0x58000050,  # ldr x16, #8 (literal)
               0x540003a0,  # b.eq
               0x94000010,  # bl
               0x14000011,  # b
               0x36000189,  # tbz
               0xb4000040,  # cbz x0
               0xd65f03c0]  # ret
        for w in ok:
            self.assertTrue(unityhooks.a64_movable(w), hex(w))
        for w in bad:
            self.assertFalse(unityhooks.a64_movable(w), hex(w))


class BranchesInto(unittest.TestCase):
    def test_a64_loop_into_entry(self):
        # entry at 0x1000: 4 instructions, then a b.ne back to 0x1008 (inside the 16 bytes the jump takes)
        words = [0xa9be57fe, 0xa9014ff4, 0x71000c1f, 0x2a0103f3]
        bne_back = 0x54000001 | (((0x1008 - 0x1010) // 4 & 0x7ffff) << 5)
        code = struct.pack('<5I', *words, bne_back)
        self.assertEqual(unityhooks.branches_into(code, 0x1000, 'x', 0x1004, 0x1010), [0x1010])
        code = struct.pack('<5I', *words, 0x54000001 | (((0x1014 - 0x1010) // 4) << 5))
        self.assertEqual(unityhooks.branches_into(code, 0x1000, 'x', 0x1004, 0x1010), [])

    def test_arm_branch_to_second_instruction(self):
        # b to 0x2004 from 0x200c: pc + 8 + imm*4 = 0x2004 -> imm = -4
        b = 0xea000000 | ((-4) & 0xffffff)
        code = struct.pack('<4I', 0xe92d4070, 0xe1a04001, 0xe3500003, b)
        self.assertEqual(unityhooks.branches_into(code, 0x2000, 'a', 0x2004, 0x2008), [0x200c])


class Fnv(unittest.TestCase):
    def test_known(self):
        self.assertEqual(unityhooks.fnv1a(b''), 0xcbf29ce484222325)
        self.assertEqual(unityhooks.fnv1a(b'a'), 0xaf63dc4c8601ec8c)


if __name__ == '__main__':
    unittest.main()
