import sys
from pathlib import Path
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'scripts'))
from m3_hotspots import static_union_charge, union_area


class GeometryTests(unittest.TestCase):
    def test_union_is_independent_of_duplicate_rectangles(self):
        self.assertEqual(union_area([(0,0,2,2),(1,0,3,2),(0,0,2,2)]),6)

    def test_blockage_in_dark_region_is_not_charged_twice(self):
        box=(0,0,10,10)
        # Half the bin is dark; the fixed rectangle lies entirely in that half.
        self.assertEqual(static_union_charge(box,[(0,0,5,10)],[(5,0,10,10)],.5),25)

    def test_overlapping_rows_and_fixed_objects_use_geometric_union(self):
        self.assertEqual(static_union_charge((0,0,10,10),[(0,0,10,10)]*2,
                                            [(0,0,5,10)]*2,.5),25)


if __name__ == '__main__':
    unittest.main()
