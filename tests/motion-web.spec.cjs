// Unified integration suite against the actual embedded workbench HTML.
// Uses mocked board APIs; never talks to a connected motor or changes Wi-Fi.
// Set PLAYWRIGHT_MODULE and BROWSER_EXECUTABLE for a custom installation.
import('../tools/motor-protocol-demo/qa-device.mjs').catch(error => {
  console.error(error);
  process.exitCode = 1;
});
