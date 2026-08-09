"""Emitters for the IFE code generator (model -> text)."""
from .cpp import emit_constants_header, emit_vtables_header
from .docs import emit_documents

__all__ = ["emit_constants_header", "emit_vtables_header", "emit_documents"]
