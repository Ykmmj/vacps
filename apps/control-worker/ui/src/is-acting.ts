/** Returns true when this node is the target of an in-flight approve/reject/test/delete. */
export function isActingOnNode(
  actingId: string | undefined | null,
  node: {
    registration?: { id?: string; backendId?: string };
    backend?: { id?: string };
  },
): boolean {
  // When idle, actingId is undefined. Pending nodes often have no backend yet, so
  // `undefined === node.backend?.id` must not count as "acting".
  if (actingId == null || actingId === '') return false;
  return (
    actingId === node.registration?.id ||
    actingId === node.backend?.id ||
    actingId === node.registration?.backendId
  );
}
