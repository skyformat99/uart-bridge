"use strict";

(function() {
  // Configuration object. Gets set up on window load
  var cfg_current = {};
  var sys_info_current = {};

  window.onload = function() {
    controlsEnableDisable(false);

    // Set "Save" button handler {{{
    document.getElementById('btn_save').onclick = function() {
      controlsEnableDisable(false);
      var els = document.getElementsByClassName('form-control');
      for (var i = 0; i < els.length; i++) {
        var id = els[i].id;
        if (!id) continue;

        if (!confSet(cfg_current, id, els[i])) {
          console.error('Unable to set', id);
        }
      }

      //delete cfg_current.misc;
      console.log("Setting...", JSON.stringify({
        config: cfg_current,
      }).length, JSON.stringify({
        config: cfg_current,
      }));
      $.ajax({
        url: '/rpc/Config.Set',
        data: JSON.stringify({
          config: cfg_current,
        }),
        type: 'POST',
        success: function(data) {
          console.log("Saving...");
          $.ajax({
            url: '/rpc/Config.Save',
            type: 'POST',
            data: JSON.stringify({"reboot": true}),
            complete: function() {
              console.log("done");
            },
          });
          setTimeout(function() {
            loadConfig();
          }, 3000);
        },
      })
    }
    // }}}

    // Set "Factory reset" button handler {{{
    document.getElementById('btn_reset').onclick = function() {
      controlsEnableDisable(false);
      $.ajax({
        url: '/rpc/FS.Remove',
        data: JSON.stringify({
          filename: "conf9.json",
        }),
        type: 'POST',
        success: function(data) {
          $.ajax({
            url: '/rpc/Sys.Reboot',
            type: 'POST',
          });
          setTimeout(function() {
            loadConfig();
          }, 3000);
        },
      })
    }

    let updateBtn = document.getElementById('btn_upload_fw');
    if (updateBtn) {
      updateBtn.onclick = function() {
        if (document.getElementById('fw_select_file').value == "") {
          alert("Plese select the firmware file");
        } else {
          controlsEnableDisable(false);
          document.getElementById('fw_upload_form').submit();
        }
      };
    }
    // }}}

    loadConfig();
  }

  // Loads config from the device and populates the form.
  function loadConfig() {
    controlsEnableDisable(false);

    $.ajax({
      url: '/rpc/Config.Get',
      complete: function() {
        controlsEnableDisable(true);
      },
      success: function(data) {
        cfg_current = data;

        document.getElementById('loading').style.display = 'none';
        console.log('Config:', cfg_current);
        var els = document.getElementsByClassName('form-control');
        for (var i = 0; i < els.length; i++) {
          var id = els[i].id;
          if (!id) continue;

          var val = confGet(cfg_current, id);
          if (val === undefined) {
            console.error('Unknown conf key:', id);
          } else {
            if (typeof val !== "boolean") {
              els[i].value = val;
            } else {
              els[i].checked = val;
            }
          }
        }
      },
    });

    $.ajax({
      url: '/rpc/Sys.GetInfo',
      complete: function() {
        controlsEnableDisable(true);
      },
      success: function(data) {
        sys_info_current = data;

        document.getElementById('loading').style.display = 'none';
        console.log('Config:', sys_info_current);
        var els = document.getElementsByClassName('form-control');
        for (var i = 0; i < els.length; i++) {
          var id = $(els[i]).data("sys-info-id");
          if (!id) continue;

          var val = confGet(sys_info_current, id);
          if (val === undefined) {
            console.error('Unknown conf key:', id);
          } else {
            if (typeof val !== "boolean") {
              els[i].value = val;
            } else {
              els[i].checked = val;
            }
          }
        }
      },
    });
  }

  // Returns a config value from a config object `cfg` by its id like
  // "wifi.ap.ssid"
  function confGet(cfg, id) {
    let items = id.split(".");
    let c = cfg;
    for (let i = 0; i < items.length; i++) {
      if (typeof c !== "object" || !(items[i] in c)) {
        return undefined;
      }

      c = c[items[i]];
    }

    return c;
  }

  // Sets a config value in a config object `cfg` by its id like
  // "wifi.ap.ssid". Value is taken from an element `el`: for text and
  // numeric values it should be a text input, for booleans it should be a
  // checkbox.
  //
  // New value should have the same type as the existing one.
  function confSet(cfg, id, el) {
    let items = id.split(".");

    if (items.length == 0) {
      return false;
    }

    let o = cfg;
    let key = items[0];

    for (let i = 1; i < items.length; i++) {
      if (typeof o !== "object" || !(key in o)) {
        return false;
      }

      o = o[key];
      key = items[i];
    }

    if (typeof o !== "object" || !(key in o)) {
      return false;
    }

    switch (typeof o[key]) {
      case "number":
        o[key] = Number(el.value);
        break;
      case "string":
        o[key] = el.value;
        break;
      case "boolean":
        o[key] = el.checked;
        break;
      default:
        return false;
    }
    return true;
  }

  // Enables or disables all form controls.
  function controlsEnableDisable(en) {
    $('.form-control').prop("disabled", !en);
    $('.button').prop("disabled", !en);
  }
})();
