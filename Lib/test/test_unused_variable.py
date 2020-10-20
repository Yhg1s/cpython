import contextlib
import unittest

class UnusedAssignmentTest(unittest.TestCase):
    def test_simple(self):
        ? = 1
        x, ?, z = range(3)
        self.assertEqual((x, z), (0, 2))
        x, ?, ?, z, ? = range(5)
        self.assertEqual((x, z), (0, 3))
        ?, x, ?, z, ? = range(5)
        self.assertEqual((x, z), (1, 3))

    def test_splat(self):
        x, *?, z = range(10)
        self.assertEqual((x, z), (0, 9))
        x, ?, ?, *?, z, ? = range(10)
        self.assertEqual((x, z), (0, 8))
        ?, x, *?, z = range(10)
        self.assertEqual((x, z), (1, 9))

    def test_for_loop(self):
        i = 0
        for ? in range(3):
            i += 1
        self.assertEqual(i, 3)
        i = 0
        for ?, ? in enumerate("abc"):
            i += 1
        self.assertEqual(i, 3)
        i = 0
        for idx, ? in enumerate("abc"):
            self.assertEqual(idx, i)
            i += 1
    
    def test_comprehension(self):
        l = [ 1 for ? in range(3) ]
        self.assertEqual(l, [1, 1, 1])
        l = [ 1 for ?, *? in enumerate(range(3)) ]
        self.assertEqual(l, [1, 1, 1])
        with self.assertRaises(TypeError):
            l = [ 1 for ?, ? in range(3) ]
        l = [ 1 for ?, ? in enumerate("abc") ]
        self.assertEqual(l, [1, 1, 1])
        l = [ idx for idx, ? in enumerate("abc") ]
        self.assertEqual(l, [0, 1, 2])
        l = list(1 for ? in range(3))
        self.assertEqual(l, [1, 1, 1])
        l = list(1 for ?, ? in enumerate("abc"))
        self.assertEqual(l, [1, 1, 1])
        l = list(idx for idx, ? in enumerate("abc"))
        self.assertEqual(l, [0, 1, 2])
        s = {1 for ? in range(3) }
        self.assertEqual(s, {1})
        s = {1 for ?, ? in enumerate("abc")}
        self.assertEqual(s, {1})
        s = {idx for ?, idx in enumerate("abc")}
        self.assertEqual(s, {"a", "b", "c"})
        d = {'k': 'v' for ? in range(3)}
        self.assertEqual(d, {'k': 'v'})
        d = {'k': 'v' for ?, ? in enumerate("abc")}
        self.assertEqual(d, {'k': 'v'})
        d = {k: None for k, ? in enumerate("abc")}
        self.assertEqual(d, {0: None, 1: None, 2: None})

    def test_with(self):
        called = False
        @contextlib.contextmanager
        def manager(n):
            nonlocal called
            called = True
            yield range(n)
        with manager(10) as ?:
            self.assertTrue(called)
        with manager(3) as (x, ?, z):
            self.assertEqual((x, z), (0, 2))
        with manager(5) as (?, x, ?, z, ?):
            self.assertEqual((x, z), (1, 3))

if __name__ == '__main__':
    unittest.main()
