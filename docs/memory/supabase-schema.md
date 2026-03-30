# Supabase Schema For Memory Persistence

This document maps the current memory runtime model onto a Supabase/Postgres schema intended for:

- Full conversation history storage across a session
- Mid-term episode storage for always-present Tier 2 recall
- Future hydration of in-memory working state from SQL
- Long-term Knowledge Graph storage (entities, relationships) with pgvector
- Long-term episodic memory storage with vector similarity search

## Scope

The schema covers three persisted surfaces:

- `Conversation`: raw ordered chat history, stored as `ConversationItem`s containing either messages or an episode stub
- `Episode`: compacted mid-term memory rows created when an ongoing episode is flushed
- `Long-term memory`: entities and relationships (Knowledge Graph) plus consolidated episodic memories (vector store), written during the sleep cycle

The SQL layer preserves:

1. The exact chat log
2. The compacted mid-term representation produced from the chat log
3. The long-term Knowledge Graph (entities + relationships) and consolidated episodic memories

## Recommended tables

### `memory_sessions`

One row per runtime session.

Suggested columns:

- `session_id text primary key`
- `user_id text not null`
- `system_prompt text not null`
- `created_at timestamptz not null`
- `ended_at timestamptz null`

### `conversation_items`

One row per ordered item in the session conversation timeline.

Suggested columns:

- `session_id text not null references memory_sessions(session_id) on delete cascade`
- `item_index bigint not null`
- `item_type text not null check (item_type in ('ongoing_episode', 'episode_stub'))`
- `episode_id text null`
- `episode_stub_content text null`
- `episode_stub_created_at timestamptz null`
- `check (...)` enforcing:
  - `ongoing_episode` rows must leave episode stub columns null
  - `episode_stub` rows must populate `episode_id`, `episode_stub_content`, and `episode_stub_created_at`
- `primary key (session_id, item_index)`

Notes:

- `item_index` preserves the exact timeline order used by the working-memory prompt.
- `episode_id` is nullable until an ongoing episode is flushed.
- The schema should enforce the same tagged-union contract as the C++ validator.
- `episode_id` should also reference `mid_term_episodes(episode_id)` once both tables exist.

### `conversation_messages`

One row per raw message inside an ongoing episode item.

Suggested columns:

- `session_id text not null`
- `item_index bigint not null`
- `message_index bigint not null`
- `turn_id text not null`
- `role text not null check (role in ('user', 'assistant'))`
- `content text not null`
- `created_at timestamptz not null`
- `primary key (session_id, item_index, message_index)`
- `foreign key (session_id, item_index) references conversation_items(session_id, item_index) on delete cascade`

Notes:

- `turn_id` comes from the gateway and is useful for traceability, replay, and dedupe.
- Keeping `message_index` explicit makes hydration deterministic.

### `mid_term_episodes`

One row per flushed episode.

Suggested columns:

- `episode_id text primary key`
- `session_id text not null references memory_sessions(session_id) on delete cascade`
- `source_item_index bigint not null`
- `tier1_detail text null`
- `tier2_summary text not null`
- `tier3_ref text not null`
- `tier3_keywords text[] not null default '{}'`
- `salience integer not null check (salience between 1 and 10)`
- `embedding extensions.vector(1536) null`
- `created_at timestamptz not null`

Notes:

- `source_item_index` links the episode back to the original conversation position.
- `embedding` uses pgvector directly so mid-term recall and long-term memory share one vector format.
- Keep it nullable because the runtime still allows embeddings to be temporarily unavailable.

### `entities`

First-class nodes in the Knowledge Graph. Represents any concept: people, tools, languages,
projects, preferences, etc. Written during the sleep cycle when mid-term episodes are consolidated.

Suggested columns:

- `entity_id text primary key`
- `user_id text not null`
- `label text not null`
- `category text not null` — lightweight disambiguation tag (e.g., "person", "tool")
- `activeness integer not null default 1 check (activeness between 1 and 10)` — determines persistent cache tier (7-10 Active, 3-6 Familiar, 1-2 Stored)
- `active_model_text text null` — rich 2-3 sentence summary, populated for Active tier entities
- `familiar_label_text text null` — name + relationship one-liner, populated for Familiar tier entities
- `name_embedding extensions.vector(1536)` — for entity lexicon matching
- `created_at timestamptz not null`
- `updated_at timestamptz not null`
- `unique (entity_id, user_id)` — enables composite FK enforcement on relationships

Notes:

- Treat `user_id` as immutable for a given `entity_id`. The transactional
  `persist_sleep_cycle_extraction(...)` path should fail if an existing entity is submitted under
  a different user rather than "moving" that entity across users.

### `relationships`

Enriched edges connecting two entities in the Knowledge Graph. Written during the sleep cycle.

Suggested columns:

- `relationship_id text primary key`
- `user_id text not null`
- `from_entity_id text not null`
- `predicate text not null`
- `to_entity_id text not null`
- `foreign key (from_entity_id, user_id) references entities(entity_id, user_id)` — composite FK enforces same-user constraint
- `foreign key (to_entity_id, user_id) references entities(entity_id, user_id)`
- `weight double precision not null default 0.0` — accumulated evidence score
- `observation_count integer not null default 0`
- `last_observed_at timestamptz not null`
- `source_episode_ids text[] not null default '{}'` — provenance (historical, not FK'd since mid-term episodes are wiped)
- `embedding extensions.vector(1536)` — pre-computed from rendered text for spreading activation
- `is_archived boolean not null default false` — archived edges kept for history, excluded from active retrieval
- `archived_at timestamptz null`
- `superseded_by text null references relationships(relationship_id)`
- `created_at timestamptz not null`

### `long_term_episodes`

Consolidated episodic memories. Created during the sleep cycle when mid-term episodes are merged
into long-term storage.

Suggested columns:

- `lte_id text primary key`
- `user_id text not null`
- `summary_full text null` — full narrative; dropped at 30+ days during decay lifecycle
- `summary_compressed text not null` — single sentence with outcome
- `keywords text[] not null default '{}'`
- `embedding extensions.vector(1536)`
- `outcome text not null default 'informational' check (outcome in ('resolved', 'abandoned', 'ongoing', 'informational'))`
- `complexity integer not null default 1 check (complexity between 1 and 10)`
- `original_episode_ids text[] not null default '{}'` — provenance (historical, not FK'd)
- `caused_by text null references long_term_episodes(lte_id)` — causal chain link
- `led_to text null references long_term_episodes(lte_id)`
- `created_at timestamptz not null`

### `long_term_episode_entities`

Junction table for bidirectional cross-links between long-term episodes and entities.

Suggested columns:

- `lte_id text not null references long_term_episodes(lte_id) on delete cascade`
- `entity_id text not null references entities(entity_id) on delete cascade`
- `primary key (lte_id, entity_id)`

## Write flow mapping

The intended runtime write sequence is:

1. Upsert `memory_sessions` on the first turn of a session
2. Append raw rows into `conversation_items` and `conversation_messages` for each message
3. When a flush completes, upsert `mid_term_episodes`
4. Persist the conversation timeline update for the flushed item

For full flushes, the store can directly replace the matching `conversation_items` row with
`item_type = 'episode_stub'`, fill `episode_id`, `episode_stub_content`, and
`episode_stub_created_at`.

For split flushes, the store calls `split_conversation_item_with_episode_stub(...)` so the suffix
validation, item-index shifting, message move, and episode-stub rewrite
happen transactionally inside Postgres.

For a sleep-cycle reset, the store calls `clear_session_working_set(...)` so the persisted
conversation timeline is deleted before the referenced `mid_term_episodes` rows are removed. Under
the current schema, deleting `conversation_items` also deletes the matching
`conversation_messages` rows via `ON DELETE CASCADE`, so a sleep-cycle reset currently drops the
persisted transcript as well. The user-scoped `user_working_memory` row is then upserted with the
now-empty live working set.

For the sleep cycle, long-term consolidation happens before the working-set clear:

1. The sleep cycle captures the current mid-term episodes
2. The store calls `persist_sleep_cycle_extraction(...)` so all long-term writes happen inside one transaction
3. Within that transaction, consolidated episodes are upserted into `long_term_episodes`
4. Extracted entities and relationships are upserted into `entities` and `relationships`
5. Entity-episode cross-links are written to `long_term_episode_entities`
6. The store calls `clear_session_working_set(...)` to wipe mid-term episodes and conversation

This mirrors the current C++ architecture:

- raw messages stay preserved across normal turn ingestion and mid-term flushes, but the current
  sleep-cycle reset deletes them with the conversation timeline
- the conversation timeline still reflects stub replacement
- mid-term episodes remain queryable as an ordered list by `created_at`
- long-term memory persists across sleep cycles and sessions

## Hydration shape

To rebuild working memory for a session:

1. Load `memory_sessions`
2. Load `conversation_items` ordered by `item_index`
3. Load `conversation_messages` ordered by `item_index,message_index`, but filter that query to
   rows whose parent `conversation_items` row is still `ongoing_episode`
4. Load `mid_term_episodes` ordered by `created_at`
5. Load `entities` for the user filtered by activeness tier to populate the persistent memory cache
   (activeness 7-10 for Active Models, 3-6 for Familiar Labels)

Steps 1-4 map directly to the `MemoryStoreSnapshot` shape in C++. Step 5 populates the
`PersistentMemoryCache` from long-term storage.

## Supabase-specific notes

- Use the server-side service role on the Isla backend only; do not ship it to the desktop client.
- Keep Row Level Security decisions simple at first. If only the backend writes memory, service-role access can own the first version.
- Keep simple writes on direct PostgREST table upserts where possible.
- Use RPC-backed SQL functions for multi-row memory mutations that need fewer round trips or
  transactional guarantees. The split-flush path now uses
  `split_conversation_item_with_episode_stub(...)`, the sleep-cycle extraction batch uses
  `persist_sleep_cycle_extraction(...)`, and the sleep-cycle reset uses
  `clear_session_working_set(...)`, for exactly that reason.
- The current schema does not preserve archived transcript rows across a sleep-cycle reset. If we
  want transcript retention later, we will need to decouple `conversation_messages` from
  `conversation_items` deletion or introduce a separate archival transcript table.

## Implementation status

Working memory and mid-term memory store methods are implemented:

- session upsert
- conversation message append
- episode upsert
- full-flush stub replacement
- split-flush RPC persistence
- sleep-cycle reset RPC persistence
- session hydration

Long-term memory store methods are implemented:

- transactional sleep-cycle extraction batch persistence
- entity upsert/list/get
- relationship upsert/list
- long-term episode upsert/list-by-entity
- episode-entity linking
