set_project("myping")
set_languages("c11")

add_rules("mode.debug", "mode.release")

target("myping")
    set_kind("binary")
    add_files("src/main.c")
