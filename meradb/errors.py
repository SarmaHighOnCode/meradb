"""
Errors raised by MeraDB.

Every layer of the engine has its own error type so that, when something
goes wrong, you immediately know WHICH stage failed:

    TokenizerError  -> the raw text had a character we don't understand
    ParseError      -> the words were valid but in the wrong order (grammar)
    ExecutionError  -> the query was valid but can't be run (no such table, etc.)
    StorageError    -> something is wrong with the files on disk
    ConnectionFailed-> the client couldn't talk to the MeraDB server
"""


class MeraDBError(Exception):
    """Base class. Catching this catches every MeraDB error."""

    stage = "MeraDB"

    def __str__(self) -> str:
        return f"[{self.stage} Galti] {self.args[0] if self.args else ''}"


class TokenizerError(MeraDBError):
    stage = "Tokenizer"


class ParseError(MeraDBError):
    stage = "Parser"


class ExecutionError(MeraDBError):
    stage = "Execution"


class StorageError(MeraDBError):
    stage = "Storage"


class ConnectionFailed(MeraDBError):
    stage = "Connection"


class ServerUnavailable(ConnectionFailed):
    """Nothing is listening at host:port (as opposed to e.g. a wrong password)."""
