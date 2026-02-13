run_case <- function(name, expr, reps = 7L) {
  ts <- numeric(reps)
  for (i in seq_len(reps)) {
    invisible(gc())
    ts[[i]] <- unname(system.time(eval(expr, envir = parent.frame()))[["elapsed"]])
  }
  c(case = name, median = median(ts), mean = mean(ts), min = min(ts), max = max(ts))
}

cases <- list(
  list(name = "empty_for", expr = quote({ for (i in 1:8e7) {} })),
  list(name = "scalar_add", expr = quote({ s <- 0.0; for (i in 1:4e7) s <- s + i; s })),
  list(name = "sin_loop", expr = quote({ x <- 0.0; for (i in 1:2e7) x <- x + sin(i); x })),
  list(name = "lookup_scalar", expr = quote({ a <- 1.0; b <- 2.0; x <- 0.0; for (i in 1:3e7) x <- x + a + b; x }))
)

rows <- lapply(cases, function(ca) run_case(ca$name, ca$expr))
out <- as.data.frame(do.call(rbind, rows), stringsAsFactors = FALSE)
out$median <- as.numeric(out$median)
out$mean <- as.numeric(out$mean)
out$min <- as.numeric(out$min)
out$max <- as.numeric(out$max)
print(out)
write.csv(out, file = commandArgs(trailingOnly = TRUE)[[1L]], row.names = FALSE)
