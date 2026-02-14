#!/usr/bin/env Rscript

mode <- Sys.getenv("TR_MODE", "strict")
allowed_prefix <- normalizePath(Sys.getenv("TR_ALLOWED_PREFIX", "."), winslash = "/", mustWork = FALSE)
strict_user <- normalizePath(Sys.getenv("TR_STRICT_USER_LIB", ""), winslash = "/", mustWork = FALSE)
allow_framework_site <- identical(Sys.getenv("TR_ALLOW_FRAMEWORK_SITE", "0"), "1")
r_home <- normalizePath(R.home(), winslash = "/", mustWork = FALSE)
home_lib <- normalizePath(file.path(R.home(), "library"), winslash = "/", mustWork = FALSE)
lib_paths <- normalizePath(.libPaths(), winslash = "/", mustWork = FALSE)

cat(sprintf("mode=%s\n", mode))
cat(sprintf("R.home()=%s\n", r_home))
cat(sprintf(".libPaths()=%s\n", paste(lib_paths, collapse = " | ")))

if (identical(mode, "strict")) {
  is_allowed <- startsWith(lib_paths, allowed_prefix) | lib_paths == strict_user | lib_paths == home_lib

  if (allow_framework_site && identical(.Platform$OS.type, "unix") && grepl("^darwin", R.version$os)) {
    minor <- strsplit(R.version$minor, ".", fixed = TRUE)[[1L]][1L]
    ver_mm <- paste(R.version$major, minor, sep = ".")
    fw_arch <- switch(R.version$arch,
      "aarch64" = "arm64",
      "x86_64" = "x86_64",
      R.version$arch
    )
    compat <- normalizePath(c(
      file.path("/Library/Frameworks/R.framework/Versions",
        paste0(ver_mm, "-", fw_arch), "Resources", "library"),
      file.path("/Library/Frameworks/R.framework/Versions",
        ver_mm, "Resources", "library")
    ), winslash = "/", mustWork = FALSE)
    is_allowed <- is_allowed | lib_paths %in% compat
  }

  blocked_prefixes <- c(
    "/Library/Frameworks/R.framework",
    "/usr/local/lib/R",
    "/opt/homebrew/lib/R"
  )
  hits_blocked <- vapply(
    lib_paths,
    function(p) any(startsWith(p, blocked_prefixes)),
    logical(1)
  )

  bad <- lib_paths[!(is_allowed) | (hits_blocked & !is_allowed)]
  if (length(bad)) {
    stop(
      sprintf(
        "strict leakage detected in .libPaths(): %s",
        paste(unique(bad), collapse = ", ")
      ),
      call. = FALSE
    )
  }
}

cat("libpath isolation check passed\n")
