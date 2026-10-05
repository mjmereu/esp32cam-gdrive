// Name of the Drive folder photos are saved into (created if it doesn't exist).
var FOLDER_NAME = "M_Test";

// Lets you open the web-app URL in a browser to confirm the deployment works.
function doGet(e) {
  return ContentService.createTextOutput("Script is live");
}

function doPost(e) {
  var data = Utilities.base64Decode(e.parameters.data);
  var nombreArchivo = Utilities.formatDate(new Date(), Session.getScriptTimeZone(), "yyyyMMdd_HHmmss") + ".jpg";
  var blob = Utilities.newBlob(data, e.parameters.mimetype, nombreArchivo);

  // Save the photo to Google Drive
  var folder, folders = DriveApp.getFoldersByName(FOLDER_NAME);
  if (folders.hasNext()) {
    folder = folders.next();
  } else {
    folder = DriveApp.createFolder(FOLDER_NAME);
  }
  folder.createFile(blob);
  return ContentService.createTextOutput("Completo");
}
