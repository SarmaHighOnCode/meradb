"""
Data types supported by MeraDB, and the rules for checking/converting values.

Each type has an English name and a Hinglish alias -- both work in BANAO TABLE:

    INT   / ANK         whole numbers          42
    FLOAT / DASHAMLAV   decimal numbers        3.14
    TEXT  / SHABD       strings                'Ravi'
    BOOL  / HAAN_NA     true / false           SACH, JHOOTH
    DATE  / TAREEKH     calendar date          '2024-01-31'

NUMBER/NUMERIC/DECIMAL are accepted as aliases of FLOAT (their precision/scale
are parsed but ignored -- see docs/LANGUAGE.md). VARCHAR2 and CHAR are further
aliases of TEXT, matching common SQL dialects.
"""

from datetime import date

from .errors import ExecutionError

INT_MIN = -(2**63)
INT_MAX = 2**63 - 1

TYPE_ALIASES = {
    "INT": "INT",
    "INTEGER": "INT",
    "ANK": "INT",
    "FLOAT": "FLOAT",
    "REAL": "FLOAT",
    "DASHAMLAV": "FLOAT",
    "NUMBER": "FLOAT",
    "NUMERIC": "FLOAT",
    "DECIMAL": "FLOAT",
    "TEXT": "TEXT",
    "STRING": "TEXT",
    "VARCHAR": "TEXT",
    "VARCHAR2": "TEXT",
    "CHAR": "TEXT",
    "SHABD": "TEXT",
    "BOOL": "BOOL",
    "BOOLEAN": "BOOL",
    "HAAN_NA": "BOOL",
    "DATE": "DATE",
    "TAREEKH": "DATE",
}


def normalize_type(name: str):
    """'ank' -> 'INT'. Returns None for unknown type names."""
    return TYPE_ALIASES.get(name.upper())


def parse_date(text: str, column: str = None) -> date:
    """'2024-01-31' -> date(2024, 1, 31). A clear Hinglish error for bad strings."""
    try:
        return date.fromisoformat(text)
    except ValueError:
        where = f"Column '{column}': " if column else ""
        raise ExecutionError(f"{where}{text!r} valid DATE nahi hai -- 'YYYY-MM-DD' format chahiye") from None


def coerce(value, type_name: str, column: str):
    """
    Check that `value` fits in a column of `type_name`, converting if it's safe.
    KHALI (None) is allowed here -- NOT NULL is checked separately.
    """
    if value is None:
        return None

    # NOTE: in Python, bool is a subclass of int (True == 1). We must check
    # for bool FIRST or SACH would sneak into INT columns as 1.
    if type_name == "INT":
        if isinstance(value, float) and value.is_integer():
            value = int(value)
        if isinstance(value, int) and not isinstance(value, bool):
            # INT is stored in 8 bytes on disk (see storage.py), so it has limits
            if not INT_MIN <= value <= INT_MAX:
                raise ExecutionError(f"Column '{column}': {value} INT ke liye bahut bada hai (8-byte limit)")
            return value
    elif type_name == "FLOAT":
        if isinstance(value, (int, float)) and not isinstance(value, bool):
            return float(value)
    elif type_name == "TEXT":
        if isinstance(value, str):
            return value
    elif type_name == "BOOL":
        if isinstance(value, bool):
            return value
    elif type_name == "DATE":
        if isinstance(value, date):
            return value
        if isinstance(value, str):
            return parse_date(value, column)

    raise ExecutionError(
        f"Column '{column}' {type_name} type ka hai, par value {format_value(value)} mili"
    )


def format_value(value) -> str:
    """How a value is shown to the user."""
    if value is None:
        return "KHALI"
    if value is True:
        return "SACH"
    if value is False:
        return "JHOOTH"
    if isinstance(value, date):
        return value.isoformat()
    if isinstance(value, str):
        return value
    if isinstance(value, float):
        # 10 significant digits: AUSAT shows 8.166666667, not 8.166666666666666
        text = f"{value:.10g}"
        return text if any(c in text for c in ".en") else text + ".0"
    return str(value)
