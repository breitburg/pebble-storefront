// PebbleKit JS for fetching apps from Rebble API

var REBBLE_API_URL = 'https://appstore-api.rebble.io/api/v1/apps/collection/all/apps?platform=all&offset=0&limit=10';
var MESSAGE_DELAY_MS = 50; // Delay between AppMessage sends to avoid overwhelming inbox

// Truncate long descriptions to first N words
function truncateDescription(text, maxWords) {
  if (!text) return '';

  var words = text.split(/\s+/);

  if (words.length <= maxWords) {
    return text;
  }

  return words.slice(0, maxWords).join(' ') + '...';
}

// Calculate days since release
function getDaysAgoText(dateString) {
  if (!dateString) return 'Unknown';

  var releaseDate = new Date(dateString);
  var today = new Date();

  // Reset time to midnight for accurate day comparison
  releaseDate.setHours(0, 0, 0, 0);
  today.setHours(0, 0, 0, 0);

  var diffTime = today - releaseDate;
  var diffDays = Math.floor(diffTime / (1000 * 60 * 60 * 24));

  if (diffDays === 0) {
    return 'Today';
  } else if (diffDays === 1) {
    return 'Yesterday';
  } else {
    return diffDays + ' days ago';
  }
}

// Fetch apps from Rebble API
function fetchApps() {
  console.log('Fetching apps from Rebble API...');

  var xhr = new XMLHttpRequest();
  xhr.open('GET', REBBLE_API_URL, true);

  xhr.onload = function () {
    // Early return for HTTP errors
    if (xhr.status !== 200) {
      console.log('HTTP error: ' + xhr.status);
      Pebble.sendAppMessage({ 'DATA_COMPLETE': 0 });
      return;
    }

    // Try to parse JSON
    var json;
    try {
      json = JSON.parse(xhr.responseText);
    } catch (e) {
      console.log('Error parsing JSON: ' + e.message);
      Pebble.sendAppMessage({ 'DATA_COMPLETE': 0 });
      return;
    }

    console.log('Received ' + json.data.length + ' apps');

    // Send each app to the watch with delays to avoid overwhelming the inbox
    json.data.forEach(function (app, index) {
      setTimeout(function () {
        var description = truncateDescription(app.description, 6);
        var daysAgo = getDaysAgoText(app.created_at);

        var message = {
          'APP_INDEX': index,
          'APP_NAME': app.title || 'Unknown',
          'APP_AUTHOR': app.author || 'Unknown',
          'APP_DESCRIPTION': description,
          'APP_HEARTS': app.hearts || 0,
          'APP_DAYS_AGO': daysAgo
        };

        console.log('Sending app ' + index + ': ' + app.title);

        Pebble.sendAppMessage(message,
          function () {
            console.log('App ' + index + ' sent successfully');
          },
          function (e) {
            console.log('Failed to send app ' + index + ': ' + e.error.message);
          }
        );
      }, index * MESSAGE_DELAY_MS);
    });

    // Signal that all data has been sent (after all messages)
    setTimeout(function () {
      Pebble.sendAppMessage({ 'DATA_COMPLETE': 1 },
        function () {
          console.log('Data complete signal sent');
        },
        function (e) {
          console.log('Failed to send complete signal: ' + e.error.message);
        }
      );
    }, (json.data.length + 1) * MESSAGE_DELAY_MS); // Wait for all apps + one more interval
  };

  xhr.onerror = function () {
    console.log('Network error occurred');
    Pebble.sendAppMessage({ 'DATA_COMPLETE': 0 });
  };

  xhr.send();
}

// Listen for when the watchapp is ready
Pebble.addEventListener('ready', function () {
  console.log('PebbleKit JS ready!');
  // Automatically fetch apps on startup
  fetchApps();
});
