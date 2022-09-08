#
# Copyright (C) 2022 Zilliqa
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program.  If not, see <https://www.gnu.org/licenses/>.

import pytest
import os
import numpy
import random as rand


@pytest.fixture(scope="session", autouse=True)
def random():
    seed = int(os.environ.get("TESTSEED", "0"))
    print("TEST RANDOM SEED IS {}".format(seed))
    rand.seed(seed)
    numpy.random.seed(seed)