-- =============================================================================
-- Working Memory + Mid-Term Memory
-- =============================================================================

-- One row per runtime session.
create table if not exists public.memory_sessions (
    session_id text primary key,
    user_id text not null,
    system_prompt text not null,
    created_at timestamptz not null,
    ended_at timestamptz
);

-- Snapshot of current working-memory state per user.
create table if not exists public.user_working_memory (
    user_id text primary key,
    session_id text not null,
    working_memory jsonb not null,
    rendered_working_memory text not null,
    updated_at timestamptz not null
);

-- Ordered conversation timeline items (ongoing episodes or episode stubs).
create table if not exists public.conversation_items (
    session_id text not null references public.memory_sessions(session_id) on delete cascade,
    item_index bigint not null,
    item_type text not null check (item_type in ('ongoing_episode', 'episode_stub')),
    episode_id text,
    episode_stub_content text,
    episode_stub_created_at timestamptz,
    check (
        (
            item_type = 'ongoing_episode'
            and episode_id is null
            and episode_stub_content is null
            and episode_stub_created_at is null
        ) or (
            item_type = 'episode_stub'
            and episode_id is not null
            and episode_stub_content is not null
            and episode_stub_created_at is not null
        )
    ),
    primary key (session_id, item_index)
);

-- Raw messages inside ongoing episode items.
create table if not exists public.conversation_messages (
    session_id text not null,
    item_index bigint not null,
    message_index bigint not null,
    turn_id text not null,
    role text not null check (role in ('user', 'assistant')),
    content text not null,
    created_at timestamptz not null,
    primary key (session_id, item_index, message_index),
    foreign key (session_id, item_index)
        references public.conversation_items(session_id, item_index)
        on delete cascade
);

-- Embedding model: gemini-embedding-2-preview with output_dimensionality=1536.
-- Native output is 3072 dims but pgvector HNSW indexes cap at 2000 for the vector type,
-- so we request half-dimensionality from the API.
create extension if not exists vector with schema extensions;

-- Flushed mid-term episodes with tiered compaction summaries.
create table if not exists public.mid_term_episodes (
    episode_id text primary key,
    session_id text not null references public.memory_sessions(session_id) on delete cascade,
    source_item_index bigint not null,
    tier1_detail text,
    tier2_summary text not null,
    tier3_ref text not null,
    tier3_keywords text[] not null default '{}',
    salience integer not null check (salience between 1 and 10),
    embedding extensions.vector(1536),
    created_at timestamptz not null
);

-- Deferred constraint alterations for cross-table foreign keys.

alter table public.conversation_items
    drop constraint if exists conversation_items_episode_id_fkey;

alter table public.conversation_items
    add constraint conversation_items_episode_id_fkey
    foreign key (episode_id)
    references public.mid_term_episodes(episode_id);

alter table public.conversation_messages
    drop constraint if exists conversation_messages_session_id_item_index_fkey;

alter table public.conversation_messages
    add constraint conversation_messages_session_id_item_index_fkey
    foreign key (session_id, item_index)
    references public.conversation_items(session_id, item_index)
    on delete cascade
    deferrable initially immediate;

alter table public.user_working_memory
    drop constraint if exists user_working_memory_session_id_fkey;

alter table public.user_working_memory
    add constraint user_working_memory_session_id_fkey
    foreign key (session_id)
    references public.memory_sessions(session_id)
    on delete cascade;

-- RPC functions for multi-row memory mutations.

create or replace function public.split_conversation_item_with_episode_stub(
    p_session_id text,
    p_conversation_item_index bigint,
    p_episode_id text,
    p_episode_stub_content text,
    p_episode_stub_created_at timestamptz,
    p_remaining_messages jsonb
)
returns void
language plpgsql
as $$
declare
    v_remaining_count bigint;
    v_target_message_count bigint;
    v_split_at_message_index bigint;
    v_item_shift_base bigint;
    v_matching_suffix_count bigint;
begin
    set constraints all deferred;

    if not exists (
        select 1
        from public.conversation_items
        where session_id = p_session_id
          and item_index = p_conversation_item_index
          and item_type = 'ongoing_episode'
    ) then
        raise exception using
            errcode = '23514',
            message = 'split flush target must be an ongoing episode';
    end if;

    v_remaining_count := jsonb_array_length(p_remaining_messages);
    if v_remaining_count < 1 then
        raise exception using
            errcode = '23514',
            message = 'remaining_ongoing_episode must contain at least one message';
    end if;

    select count(*)
    into v_target_message_count
    from public.conversation_messages
    where session_id = p_session_id
      and item_index = p_conversation_item_index;

    if v_target_message_count < v_remaining_count then
        raise exception using
            errcode = '23514',
            message = 'remaining_ongoing_episode does not match persisted conversation item';
    end if;

    v_split_at_message_index := v_target_message_count - v_remaining_count;
    if v_split_at_message_index < 2 then
        raise exception using
            errcode = '23514',
            message = 'split flush requires at least two completed messages before the remaining suffix';
    end if;

    select count(*)
    into v_matching_suffix_count
    from jsonb_array_elements(p_remaining_messages) with ordinality as expected(message, ordinality)
    join public.conversation_messages persisted
      on persisted.session_id = p_session_id
     and persisted.item_index = p_conversation_item_index
     and persisted.message_index = v_split_at_message_index + expected.ordinality - 1
    where persisted.role = expected.message ->> 'role'
      and persisted.content = expected.message ->> 'content'
      and persisted.created_at = ((expected.message ->> 'created_at')::timestamptz);

    if v_matching_suffix_count <> v_remaining_count then
        raise exception using
            errcode = '23514',
            message = 'remaining_ongoing_episode does not match persisted conversation messages';
    end if;

    select coalesce(max(item_index), p_conversation_item_index) + 1
    into v_item_shift_base
    from public.conversation_items
    where session_id = p_session_id;

    update public.conversation_items
    set item_index = item_index + v_item_shift_base
    where session_id = p_session_id
      and item_index > p_conversation_item_index;

    update public.conversation_messages
    set item_index = item_index + v_item_shift_base
    where session_id = p_session_id
      and item_index > p_conversation_item_index;

    insert into public.conversation_items(
        session_id,
        item_index,
        item_type,
        episode_id,
        episode_stub_content,
        episode_stub_created_at
    ) values (
        p_session_id,
        p_conversation_item_index + 1,
        'ongoing_episode',
        null,
        null,
        null
    );

    update public.conversation_items
    set item_index = item_index - v_item_shift_base + 1
    where session_id = p_session_id
      and item_index > p_conversation_item_index + v_item_shift_base;

    update public.conversation_messages
    set item_index = item_index - v_item_shift_base + 1
    where session_id = p_session_id
      and item_index > p_conversation_item_index + v_item_shift_base;

    update public.conversation_messages
    set item_index = p_conversation_item_index + 1,
        message_index = message_index - v_split_at_message_index
    where session_id = p_session_id
      and item_index = p_conversation_item_index
      and message_index >= v_split_at_message_index;

    update public.conversation_items
    set item_type = 'episode_stub',
        episode_id = p_episode_id,
        episode_stub_content = p_episode_stub_content,
        episode_stub_created_at = p_episode_stub_created_at
    where session_id = p_session_id
      and item_index = p_conversation_item_index;
end;
$$;

create or replace function public.clear_session_working_set(
    p_session_id text
)
returns void
language plpgsql
as $$
begin
    -- NOTICE: This is a full working-set reset. Deleting conversation_items also deletes the
    -- matching conversation_messages rows via the ON DELETE CASCADE foreign key, so the persisted
    -- raw transcript for the session is removed here as well.
    delete from public.conversation_items
    where session_id = p_session_id;

    delete from public.mid_term_episodes
    where session_id = p_session_id;
end;
$$;

create or replace function public.persist_sleep_cycle_extraction(
    p_extraction jsonb
)
returns void
language plpgsql
as $$
declare
    v_conflicting_entity_id text;
    v_existing_entity_user_id text;
    v_incoming_entity_user_id text;
begin
    if p_extraction is null then
        raise exception using
            errcode = '23514',
            message = 'p_extraction must not be null';
    end if;

    set constraints all deferred;

    -- NOTICE: entity_id ownership is immutable. If an entity already exists for a different
    -- user, fail explicitly before any writes so the caller gets a clear invariant violation
    -- instead of a later composite-FK failure from relationships.
    select
        existing.entity_id,
        existing.user_id,
        incoming.entity_json ->> 'user_id'
    into
        v_conflicting_entity_id,
        v_existing_entity_user_id,
        v_incoming_entity_user_id
    from jsonb_array_elements(coalesce(p_extraction -> 'entities', '[]'::jsonb)) as incoming(entity_json)
    join public.entities existing
      on existing.entity_id = incoming.entity_json ->> 'entity_id'
    where existing.user_id <> incoming.entity_json ->> 'user_id'
    limit 1;

    if v_conflicting_entity_id is not null then
        raise exception using
            errcode = '23514',
            message = format(
                'entity_id %s already belongs to user_id %s and cannot be moved to user_id %s',
                v_conflicting_entity_id,
                v_existing_entity_user_id,
                v_incoming_entity_user_id
            );
    end if;

    insert into public.entities(
        entity_id,
        user_id,
        label,
        category,
        activeness,
        active_model_text,
        familiar_label_text,
        name_embedding,
        created_at,
        updated_at
    )
    select
        entity_json ->> 'entity_id',
        entity_json ->> 'user_id',
        entity_json ->> 'label',
        entity_json ->> 'category',
        (entity_json ->> 'activeness')::integer,
        entity_json ->> 'active_model_text',
        entity_json ->> 'familiar_label_text',
        case
            when entity_json -> 'name_embedding' is null
                 or entity_json -> 'name_embedding' = 'null'::jsonb then null
            else (entity_json ->> 'name_embedding')::extensions.vector
        end,
        (entity_json ->> 'created_at')::timestamptz,
        (entity_json ->> 'updated_at')::timestamptz
    from jsonb_array_elements(coalesce(p_extraction -> 'entities', '[]'::jsonb)) as entity_json
    on conflict (entity_id) do update
    set label = excluded.label,
        category = excluded.category,
        activeness = excluded.activeness,
        active_model_text = excluded.active_model_text,
        familiar_label_text = excluded.familiar_label_text,
        name_embedding = excluded.name_embedding,
        updated_at = excluded.updated_at;

    insert into public.relationships(
        relationship_id,
        user_id,
        from_entity_id,
        predicate,
        to_entity_id,
        weight,
        observation_count,
        last_observed_at,
        source_episode_ids,
        embedding,
        is_archived,
        archived_at,
        superseded_by,
        created_at
    )
    select
        relationship_json ->> 'relationship_id',
        relationship_json ->> 'user_id',
        relationship_json ->> 'from_entity_id',
        relationship_json ->> 'predicate',
        relationship_json ->> 'to_entity_id',
        (relationship_json ->> 'weight')::double precision,
        (relationship_json ->> 'observation_count')::integer,
        (relationship_json ->> 'last_observed_at')::timestamptz,
        coalesce(
            array(
                select jsonb_array_elements_text(
                    coalesce(relationship_json -> 'source_episode_ids', '[]'::jsonb)
                )
            ),
            '{}'::text[]
        ),
        case
            when relationship_json -> 'embedding' is null
                 or relationship_json -> 'embedding' = 'null'::jsonb then null
            else (relationship_json ->> 'embedding')::extensions.vector
        end,
        coalesce((relationship_json ->> 'is_archived')::boolean, false),
        (relationship_json ->> 'archived_at')::timestamptz,
        relationship_json ->> 'superseded_by',
        (relationship_json ->> 'created_at')::timestamptz
    from jsonb_array_elements(coalesce(p_extraction -> 'relationships', '[]'::jsonb))
        as relationship_json
    on conflict (relationship_id) do update
    set user_id = excluded.user_id,
        from_entity_id = excluded.from_entity_id,
        predicate = excluded.predicate,
        to_entity_id = excluded.to_entity_id,
        weight = excluded.weight,
        observation_count = excluded.observation_count,
        last_observed_at = excluded.last_observed_at,
        source_episode_ids = excluded.source_episode_ids,
        embedding = excluded.embedding,
        is_archived = excluded.is_archived,
        archived_at = excluded.archived_at,
        superseded_by = excluded.superseded_by;

    insert into public.long_term_episodes(
        lte_id,
        user_id,
        summary_full,
        summary_compressed,
        keywords,
        embedding,
        outcome,
        complexity,
        original_episode_ids,
        caused_by,
        led_to,
        created_at
    )
    select
        episode_json ->> 'lte_id',
        episode_json ->> 'user_id',
        episode_json ->> 'summary_full',
        episode_json ->> 'summary_compressed',
        coalesce(
            array(
                select jsonb_array_elements_text(
                    coalesce(episode_json -> 'keywords', '[]'::jsonb)
                )
            ),
            '{}'::text[]
        ),
        case
            when episode_json -> 'embedding' is null
                 or episode_json -> 'embedding' = 'null'::jsonb then null
            else (episode_json ->> 'embedding')::extensions.vector
        end,
        coalesce(episode_json ->> 'outcome', 'informational'),
        (episode_json ->> 'complexity')::integer,
        coalesce(
            array(
                select jsonb_array_elements_text(
                    coalesce(episode_json -> 'original_episode_ids', '[]'::jsonb)
                )
            ),
            '{}'::text[]
        ),
        episode_json ->> 'caused_by',
        episode_json ->> 'led_to',
        (episode_json ->> 'created_at')::timestamptz
    from jsonb_array_elements(coalesce(p_extraction -> 'long_term_episodes', '[]'::jsonb))
        as episode_json
    on conflict (lte_id) do update
    set user_id = excluded.user_id,
        summary_full = excluded.summary_full,
        summary_compressed = excluded.summary_compressed,
        keywords = excluded.keywords,
        embedding = excluded.embedding,
        outcome = excluded.outcome,
        complexity = excluded.complexity,
        original_episode_ids = excluded.original_episode_ids,
        caused_by = excluded.caused_by,
        led_to = excluded.led_to;

    insert into public.long_term_episode_entities(
        lte_id,
        entity_id
    )
    select
        link_json ->> 'lte_id',
        entity_id_text.value
    from jsonb_array_elements(coalesce(p_extraction -> 'long_term_episode_entity_links',
                                       '[]'::jsonb)) as link_json
    cross join lateral jsonb_array_elements_text(coalesce(link_json -> 'entity_ids', '[]'::jsonb))
        as entity_id_text(value)
    on conflict (lte_id, entity_id) do nothing;
end;
$$;

-- Working memory + mid-term memory indexes.

create index if not exists conversation_items_session_order_idx
    on public.conversation_items (session_id, item_index);

create index if not exists conversation_messages_session_item_order_idx
    on public.conversation_messages (session_id, item_index, message_index);

create index if not exists mid_term_episodes_session_created_at_idx
    on public.mid_term_episodes (session_id, created_at);

create index if not exists user_working_memory_session_id_idx
    on public.user_working_memory (session_id);

-- =============================================================================
-- Long-Term Memory: Knowledge Graph + Episodic Vector Store
-- =============================================================================

-- Entities: first-class nodes in the Knowledge Graph.
create table if not exists public.entities (
    entity_id           text primary key,
    user_id             text not null,
    label               text not null,
    category            text not null,
    activeness          integer not null default 1
                        check (activeness between 1 and 10),
    active_model_text   text,
    familiar_label_text text,
    name_embedding      extensions.vector(1536),
    created_at          timestamptz not null,
    updated_at          timestamptz not null,
    unique (entity_id, user_id)
);

-- Relationships: enriched edges connecting two entities.
create table if not exists public.relationships (
    relationship_id     text primary key,
    user_id             text not null,
    from_entity_id      text not null,
    predicate           text not null,
    to_entity_id        text not null,
    foreign key (from_entity_id, user_id) references public.entities(entity_id, user_id),
    foreign key (to_entity_id, user_id) references public.entities(entity_id, user_id),
    weight              double precision not null default 0.0,
    observation_count   integer not null default 0,
    last_observed_at    timestamptz not null,
    source_episode_ids  text[] not null default '{}',
    embedding           extensions.vector(1536),
    is_archived         boolean not null default false,
    archived_at         timestamptz,
    superseded_by       text references public.relationships(relationship_id),
    created_at          timestamptz not null
);

-- Long-term episodes: consolidated episodic memories.
create table if not exists public.long_term_episodes (
    lte_id                  text primary key,
    user_id                 text not null,
    summary_full            text,
    summary_compressed      text not null,
    keywords                text[] not null default '{}',
    embedding               extensions.vector(1536),
    outcome                 text not null default 'informational'
                            check (outcome in ('resolved', 'abandoned', 'ongoing', 'informational')),
    complexity              integer not null default 1
                            check (complexity between 1 and 10),
    original_episode_ids    text[] not null default '{}',
    caused_by               text references public.long_term_episodes(lte_id),
    led_to                  text references public.long_term_episodes(lte_id),
    created_at              timestamptz not null
);

-- Junction table: bidirectional cross-links between long-term episodes and entities.
create table if not exists public.long_term_episode_entities (
    lte_id      text not null references public.long_term_episodes(lte_id) on delete cascade,
    entity_id   text not null references public.entities(entity_id) on delete cascade,
    primary key (lte_id, entity_id)
);

-- Entity indexes.
create index if not exists entities_user_id_idx
    on public.entities (user_id);
create index if not exists entities_user_activeness_idx
    on public.entities (user_id, activeness);

create index if not exists relationships_user_id_idx
    on public.relationships (user_id);

-- Relationship indexes (partial: exclude archived edges from active lookups).
create index if not exists relationships_from_entity_active_idx
    on public.relationships (from_entity_id) where not is_archived;
create index if not exists relationships_to_entity_active_idx
    on public.relationships (to_entity_id) where not is_archived;

-- Vector similarity indexes (HNSW for better recall).
create index if not exists entities_name_embedding_idx
    on public.entities using hnsw (name_embedding extensions.vector_cosine_ops);
create index if not exists relationships_embedding_idx
    on public.relationships using hnsw (embedding extensions.vector_cosine_ops);
create index if not exists long_term_episodes_embedding_idx
    on public.long_term_episodes using hnsw (embedding extensions.vector_cosine_ops);

-- Episode-entity cross-link reverse index.
create index if not exists long_term_episode_entities_entity_idx
    on public.long_term_episode_entities (entity_id);

-- Long-term episode lookups.
create index if not exists long_term_episodes_user_id_idx
    on public.long_term_episodes (user_id);

-- Cosine similarity search over long-term episode embeddings.
-- Returns the top-k most similar episodes for a given user, ordered by descending similarity.
-- Episodes without an embedding are excluded.
create or replace function public.search_long_term_episodes_by_embedding(
    p_user_id text,
    p_query_embedding extensions.vector,
    p_top_k integer default 10
)
returns setof public.long_term_episodes
language sql stable
as $$
    select *
    from public.long_term_episodes
    where user_id = p_user_id
      and embedding is not null
    order by embedding <=> p_query_embedding
    limit p_top_k;
$$;
