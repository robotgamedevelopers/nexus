import unittest


class Skips(unittest.TestCase):
    def test_passes(self):
        pass

    @unittest.skip("planted skip")
    def test_skips(self):
        pass
