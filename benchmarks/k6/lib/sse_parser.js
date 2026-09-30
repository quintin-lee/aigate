/**
 * Utility for parsing text/event-stream chunks and extracting SSE event objects.
 */
export function parseSSELines(rawText) {
  const lines = rawText.split('\n');
  const events = [];
  let currentEvent = 'message';

  for (let line of lines) {
    line = line.trim();
    if (!line) continue;
    if (line.startsWith('event:')) {
      currentEvent = line.substring(6).trim();
    } else if (line.startsWith('data:')) {
      const dataStr = line.substring(5).trim();
      events.push({ event: currentEvent, data: dataStr });
    }
  }
  return events;
}
