settings = {
	repeatrate = 200
};

-- missing:
-- menu
--  (setup... -> reset layout, server (discover, specify), key config (reset, ...)
--  (connect)
--
-- map events to tables
-- remote control (add client debug window)
--
-- issues:
--
--

net_dispatch = {};
net_dispatch["key"] = function() settings.next_key = nil; end
net_dispatch["value"] = function() end
net_dispatch["begin_item"] = function() settings.cur_item = {}; end
net_dispatch["end_item"]   = function()
-- update selection	
end

function net_event(source, tbl)
	if (tbl.kind == "message") then
		if (tbl.message == "begin_item") then
			settings.cur_item = {};
		elseif (tbl.message
		
	end
end

function gridle_remote()
	system_load("scripts/resourcefinder.lua")();
	system_load("scripts/keyconf.lua")();
	system_load("scripts/3dsupport.lua")();
	system_load("scripts/listview.lua")();
	system_load("remote_config.lua")();

-- rREMOTE_ESCAPE gets remapped to MENU_ESCAPE
	local keylabels = { "rLOCAL_MENU", "rMENU_TOGGLE", "rREMOTE_ESCAPE", "rMENU_LEFT", "rMENU_RIGHT", "rMENU_UP", "rMENU_DOWN", "rMENU_SELECT", "rLAUNCH",
		"aMOUSE_X", "aMOUSE_Y", " CONTEXT", " FLAG_FAVORITE", " RANDOM_GAME", " OSD_KEYBOARD", " QUICKSAVE", " QUICKLOAD"};

-- prepare a keyconfig that support the specified set of labels (could be nil and get a default one)
	keyconfig = keyconf_create(keylabels);

-- if active, then there's nothing needed to be done, else we need a UI to help.
	if (keyconfig.active == false) then
		keyconfig:to_front();
		oldinput = gridle_remote_input;
		gridle_remote_input = function(iotbl)
			if (keyconfig:input(iotbl) == true) then
				gridle_remote_input = oldinput;
				gridleremote_customview();
			end
			end;
	else
		gridleremote_customview();
	end

	settings.server = net_open(net_event);
end

function gridle_remote_dispatchinput(iotbl, override)
	local restbl = override and override or keyconfig:match(iotbl);
	
	if (restbl or iotbl.kind == "analog") then
		for ind,val in pairs(restbl) do

			if (settings.iodispatch[val] and iotbl.active) then
				settings.iodispatch[val](restbl, iotbl);

			elseif settings.server then
				local msgstr = iotbl.active and "press:" or "release:";
				msgstr = msgstr .. val;
				net_push(settings.server, msgstr);
			end

		end
	end
end

gridle_remote_input = gridle_remote_dispatchinput;