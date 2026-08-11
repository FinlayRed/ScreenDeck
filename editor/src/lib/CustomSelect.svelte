<script lang="ts">
  import { onDestroy, onMount, tick } from "svelte";
  import Check from "@lucide/svelte/icons/check";
  import ChevronDown from "@lucide/svelte/icons/chevron-down";

  type SelectOption = { value: string; label: string; disabled?: boolean };

  export let value = "";
  export let options: readonly SelectOption[] = [];
  export let onChange: (value: string) => void = () => {};
  export let id: string | undefined = undefined;
  export let ariaLabel: string | undefined = undefined;
  export let disabled = false;
  export let className = "";

  const listboxId = `select-${Math.random().toString(36).slice(2)}`;
  let root: HTMLDivElement;
  let trigger: HTMLButtonElement;
  let menu: HTMLDivElement;
  let open = false;
  let activeIndex = 0;
  let menuStyle = "";
  let typeahead = "";
  let typeaheadTimer: ReturnType<typeof setTimeout> | undefined;

  $: selectedIndex = Math.max(0, options.findIndex((option) => option.value === value));
  $: selectedOption = options[selectedIndex] ?? options[0];

  function enabledIndex(start: number, direction: 1 | -1) {
    if (!options.length) return 0;
    let index = (start + options.length) % options.length;
    for (let count = 0; count < options.length; count += 1) {
      if (!options[index]?.disabled) return index;
      index = (index + direction + options.length) % options.length;
    }
    return selectedIndex;
  }

  async function positionMenu() {
    await tick();
    if (!open || !trigger || !menu) return;
    const rect = trigger.getBoundingClientRect();
    const menuHeight = Math.min(menu.scrollHeight, 224);
    const menuWidth = Math.max(rect.width, menu.scrollWidth);
    const left = Math.max(8, Math.min(rect.left, window.innerWidth - menuWidth - 8));
    const spaceBelow = window.innerHeight - rect.bottom - 8;
    const openAbove = spaceBelow < menuHeight + 5 && rect.top > spaceBelow;
    const top = openAbove
      ? Math.max(8, rect.top - menuHeight - 5)
      : Math.min(window.innerHeight - menuHeight - 8, rect.bottom + 5);
    menuStyle = `left:${left}px;top:${top}px;width:${menuWidth}px`;
    menu.querySelector<HTMLElement>(`[data-option-index="${activeIndex}"]`)?.scrollIntoView({ block: "nearest" });
  }

  async function setOpen(next: boolean) {
    if (disabled) return;
    open = next;
    if (open) {
      activeIndex = enabledIndex(selectedIndex, 1);
      await positionMenu();
    }
  }

  async function choose(index: number) {
    const option = options[index];
    if (!option || option.disabled) return;
    onChange(option.value);
    open = false;
    await tick();
    trigger?.focus();
  }

  function moveActive(direction: 1 | -1) {
    activeIndex = enabledIndex(activeIndex + direction, direction);
    tick().then(() => menu?.querySelector<HTMLElement>(`[data-option-index="${activeIndex}"]`)?.scrollIntoView({ block: "nearest" }));
  }

  function triggerKeydown(event: KeyboardEvent) {
    if (disabled) return;
    if (event.key === "ArrowDown" || event.key === "ArrowUp") {
      event.preventDefault();
      if (!open) void setOpen(true);
      else moveActive(event.key === "ArrowDown" ? 1 : -1);
      return;
    }
    if (event.key === "Home" || event.key === "End") {
      if (!open) return;
      event.preventDefault();
      activeIndex = enabledIndex(event.key === "Home" ? 0 : options.length - 1, event.key === "Home" ? 1 : -1);
      return;
    }
    if (event.key === "Enter" || event.key === " ") {
      event.preventDefault();
      if (open) void choose(activeIndex);
      else void setOpen(true);
      return;
    }
    if (event.key === "Escape" && open) {
      event.preventDefault();
      open = false;
      return;
    }
    if (event.key === "Tab") {
      open = false;
      return;
    }
    if (event.key.length === 1 && !event.ctrlKey && !event.metaKey && !event.altKey) {
      typeahead += event.key.toLowerCase();
      clearTimeout(typeaheadTimer);
      typeaheadTimer = setTimeout(() => typeahead = "", 600);
      const start = open ? activeIndex + 1 : selectedIndex + 1;
      const ordered = [...options.slice(start), ...options.slice(0, start)];
      const match = ordered.find((option) => !option.disabled && option.label.toLowerCase().startsWith(typeahead));
      if (match) {
        activeIndex = options.indexOf(match);
        if (!open) void choose(activeIndex);
      }
    }
  }

  function focusOut(event: FocusEvent) {
    if (event.relatedTarget instanceof Node && root.contains(event.relatedTarget)) return;
    open = false;
  }

  function documentPointerDown(event: PointerEvent) {
    if (open && event.target instanceof Node && !root.contains(event.target)) open = false;
  }

  function closeOnScroll(event: Event) {
    if (event.target instanceof Node && root.contains(event.target)) return;
    open = false;
  }

  function closeOnResize() { open = false; }

  onMount(() => {
    document.addEventListener("pointerdown", documentPointerDown, true);
    document.addEventListener("scroll", closeOnScroll, true);
    window.addEventListener("resize", closeOnResize);
  });

  onDestroy(() => {
    clearTimeout(typeaheadTimer);
    document.removeEventListener("pointerdown", documentPointerDown, true);
    document.removeEventListener("scroll", closeOnScroll, true);
    window.removeEventListener("resize", closeOnResize);
  });
</script>

<div class={`custom-select ${className}`} class:open bind:this={root} on:focusout={focusOut}>
  <button
    bind:this={trigger}
    {id}
    type="button"
    class="custom-select-trigger"
    aria-label={ariaLabel}
    role="combobox"
    aria-haspopup="listbox"
    aria-expanded={open}
    aria-controls={open ? listboxId : undefined}
    aria-activedescendant={open ? `${listboxId}-${activeIndex}` : undefined}
    {disabled}
    on:click={() => setOpen(!open)}
    on:keydown={triggerKeydown}
  >
    <span>{selectedOption?.label ?? "Select"}</span>
    <ChevronDown size={14}/>
  </button>

  {#if open}
    <div bind:this={menu} id={listboxId} class="custom-select-menu" role="listbox" aria-label={ariaLabel} style={menuStyle}>
      {#each options as option, index}
        <button
          id={`${listboxId}-${index}`}
          type="button"
          role="option"
          aria-selected={option.value === value}
          class:selected={option.value === value}
          class:active={index === activeIndex}
          disabled={option.disabled}
          data-option-index={index}
          on:mouseenter={() => activeIndex = index}
          on:click={() => choose(index)}
        >
          <span>{option.label}</span>
          {#if option.value === value}<Check size={13}/>{/if}
        </button>
      {/each}
    </div>
  {/if}
</div>
