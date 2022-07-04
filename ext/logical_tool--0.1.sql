-- Complain if script is sourced in psql, rather than via CREATE EXTENSION.
\echo Use "CREATE EXTENSION logical_tool" to load this file. \quit

CREATE OR REPLACE FUNCTION logical_tool_key_schema(name) RETURNS text
    AS 'logical_tool', 'logical_tool_key_schema' LANGUAGE C VOLATILE STRICT;

CREATE OR REPLACE FUNCTION logical_tool_row_schema(name) RETURNS text
    AS 'logical_tool', 'logical_tool_row_schema' LANGUAGE C VOLATILE STRICT;

CREATE OR REPLACE FUNCTION logical_tool_frame_schema() RETURNS text
    AS 'logical_tool', 'logical_tool_frame_schema' LANGUAGE C VOLATILE STRICT;

DROP DOMAIN IF EXISTS logical_tool_error_policy;
CREATE DOMAIN logical_tool_error_policy AS text
    CONSTRAINT logical_tool_error_policy_valid CHECK (VALUE IN (
        -- these values should match the constants defined in protocol.h
        'log',
        'exit'
    ));

CREATE OR REPLACE FUNCTION logical_tool_export(
        table_pattern text    DEFAULT '%',
        allow_unkeyed boolean DEFAULT false,
        error_policy logical_tool_error_policy DEFAULT 'exit',
        table_list_path text DEFAULT 'table_list.conf'
    ) RETURNS setof bytea
    AS 'logical_tool', 'logical_tool_export' LANGUAGE C VOLATILE STRICT;
