const path = require('node:path');
exports.app = { isPackaged: false, getAppPath: () => path.resolve(__dirname, '../../../..') };
