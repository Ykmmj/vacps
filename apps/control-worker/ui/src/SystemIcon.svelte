<script lang="ts">
  import ServerIcon from '@lucide/svelte/icons/server';
  import {
    siAlpinelinux,
    siArchlinux,
    siCentos,
    siDebian,
    siFedora,
    siOpensuse,
    siRockylinux,
    siUbuntu,
    type SimpleIcon,
  } from 'simple-icons';

  type Props = {
    distribution?: string;
  };

  let { distribution }: Props = $props();

  function iconForDistribution(value?: string): SimpleIcon | undefined {
    const normalized = value?.trim().toLocaleLowerCase() ?? '';

    if (normalized.includes('ubuntu')) return siUbuntu;
    if (normalized.includes('debian')) return siDebian;
    if (normalized.includes('alpine')) return siAlpinelinux;
    if (normalized.includes('arch')) return siArchlinux;
    if (normalized.includes('fedora')) return siFedora;
    if (normalized.includes('centos')) return siCentos;
    if (normalized.includes('rocky')) return siRockylinux;
    if (normalized.includes('opensuse') || normalized.includes('open suse')) return siOpensuse;

    return undefined;
  }

  const icon = $derived(iconForDistribution(distribution));
</script>

{#if icon}
  <svg aria-hidden="true" fill="currentColor" focusable="false" viewBox="0 0 24 24">
    <path d={icon.path} />
  </svg>
{:else}
  <ServerIcon aria-hidden="true" />
{/if}
