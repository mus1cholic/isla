create table if not exists public.memory_sessions (
    session_id text primary key,
    user_id text not null,
    system_prompt text not null,
    created_at timestamptz not null,
    ended_at timestamptz
);

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

create table if not exists public.mid_term_episodes (
    episode_id text primary key,
    session_id text not null references public.memory_sessions(session_id) on delete cascade,
    source_item_index bigint not null,
    tier1_detail text,
    tier2_summary text not null,
    tier3_ref text not null,
    tier3_keywords text[] not null default '{}',
    salience integer not null check (salience between 1 and 10),
    embedding jsonb not null default '[]'::jsonb,
    created_at timestamptz not null
);

alter table public.conversation_items
    drop constraint if exists conversation_items_episode_id_fkey;

alter table public.conversation_items
    add constraint conversation_items_episode_id_fkey
    foreign key (episode_id)
    references public.mid_term_episodes(episode_id);

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
    v_item_index bigint;
    v_message_index bigint;
    v_expected_message jsonb;
    v_persisted_role text;
    v_persisted_content text;
    v_persisted_created_at timestamptz;
begin
    if not exists (
        select 1
        from public.conversation_items
        where session_id = p_session_id
          and item_index = p_conversation_item_index
          and item_type = 'ongoing_episode'
    ) then
        raise exception 'split flush target must be an ongoing episode';
    end if;

    v_remaining_count := jsonb_array_length(p_remaining_messages);
    if v_remaining_count < 1 then
        raise exception 'remaining_ongoing_episode must contain at least one message';
    end if;

    select count(*)
    into v_target_message_count
    from public.conversation_messages
    where session_id = p_session_id
      and item_index = p_conversation_item_index;

    if v_target_message_count < v_remaining_count then
        raise exception 'remaining_ongoing_episode does not match persisted conversation item';
    end if;

    v_split_at_message_index := v_target_message_count - v_remaining_count;
    if v_split_at_message_index < 2 then
        raise exception 'split flush requires at least two completed messages before the remaining suffix';
    end if;

    for v_message_index in 0..(v_remaining_count - 1) loop
        v_expected_message := p_remaining_messages -> v_message_index;

        select role, content, created_at
        into v_persisted_role, v_persisted_content, v_persisted_created_at
        from public.conversation_messages
        where session_id = p_session_id
          and item_index = p_conversation_item_index
          and message_index = v_split_at_message_index + v_message_index;

        if v_persisted_role is null or
           v_persisted_role <> (v_expected_message ->> 'role') or
           v_persisted_content <> (v_expected_message ->> 'content') or
           v_persisted_created_at <> ((v_expected_message ->> 'created_at')::timestamptz) then
            raise exception 'remaining_ongoing_episode does not match persisted conversation messages';
        end if;
    end loop;

    for v_item_index in
        select item_index
        from public.conversation_items
        where session_id = p_session_id
          and item_index > p_conversation_item_index
        order by item_index desc
    loop
        update public.conversation_items
        set item_index = v_item_index + 1
        where session_id = p_session_id
          and item_index = v_item_index;
    end loop;

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

    for v_item_index in
        select distinct item_index
        from public.conversation_messages
        where session_id = p_session_id
          and item_index > p_conversation_item_index
        order by item_index desc
    loop
        update public.conversation_messages
        set item_index = v_item_index + 1
        where session_id = p_session_id
          and item_index = v_item_index;
    end loop;

    for v_message_index in 0..(v_remaining_count - 1) loop
        update public.conversation_messages
        set item_index = p_conversation_item_index + 1,
            message_index = v_message_index
        where session_id = p_session_id
          and item_index = p_conversation_item_index
          and message_index = v_split_at_message_index + v_message_index;
    end loop;

    delete from public.conversation_messages
    where session_id = p_session_id
      and item_index = p_conversation_item_index;

    update public.conversation_items
    set item_type = 'episode_stub',
        episode_id = p_episode_id,
        episode_stub_content = p_episode_stub_content,
        episode_stub_created_at = p_episode_stub_created_at
    where session_id = p_session_id
      and item_index = p_conversation_item_index;
end;
$$;

create index if not exists conversation_items_session_order_idx
    on public.conversation_items (session_id, item_index);

create index if not exists conversation_messages_session_item_order_idx
    on public.conversation_messages (session_id, item_index, message_index);

create index if not exists mid_term_episodes_session_created_at_idx
    on public.mid_term_episodes (session_id, created_at);
