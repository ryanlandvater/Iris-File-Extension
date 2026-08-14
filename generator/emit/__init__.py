"""Emitters for the IFE code generator (model -> text)."""
from .cpp import emit_blocks_header, emit_validation_header, emit_validation_source
from .docs import emit_documents

__all__ = ["emit_blocks_header", "emit_validation_header", "emit_validation_source", "emit_documents"]
