// The dialogs of the original, one function each: build from the resource layout, fill from the
// settings, and apply on OK. They return true when something was changed.
#pragma once
class MainWindow;

namespace dlg {
bool setup(MainWindow *w);            // Interface -> Setup
bool options(MainWindow *w);          // Options -> Options
bool general(MainWindow *w);          // Options -> General
bool mail(MainWindow *w);             // Options -> SMTP/Email
bool filters(MainWindow *w);          // Filters -> Filter List
bool filterOptions(MainWindow *w);    // Filters -> Filter Options
bool clearScreen(MainWindow *w);      // Display -> Clear Screen
bool colors(MainWindow *w);           // Display -> Colors
bool screenOptions(MainWindow *w);    // Display -> Screen Options
bool scrollback(MainWindow *w);       // Display -> Scrollback
bool systemTray(MainWindow *w);       // Display -> Systemtray
bool statistics(MainWindow *w);       // Monitor -> Statistics
bool logfile(MainWindow *w);          // File -> Open/Close Logfile
bool debug(MainWindow *w);            // Help -> Debug Information
bool about(MainWindow *w);            // Help -> About
}  // namespace dlg
