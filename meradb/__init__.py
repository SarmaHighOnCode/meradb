"""MeraDB -- a tiny database engine with a Hinglish query language, built from scratch."""

__version__ = "1.0.0"  # defined first: other modules import it while the package loads

from .engine import Engine, Result  # noqa: E402
from .errors import MeraDBError  # noqa: E402

__all__ = ["Engine", "Result", "MeraDBError", "__version__"]
