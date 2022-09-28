set_project("myping")
set_languages("gnu11")

add_rules("mode.debug", "mode.release")

target("myping")
    set_kind("binary")
    add_files("ping.c")
