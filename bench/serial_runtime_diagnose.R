## Serial runtime diagnosis benchmark.
##
## Usage:
##   R --vanilla -q -f bench/serial_runtime_diagnose.R --args out.rds label
##
## Optional env vars:
## - DIAG_SMALL_ITERS (default 5)
## - DIAG_HEAVY_ITERS (default 3)

parse_int <- function(x, default) {
  if (!nzchar(x)) return(default)
  as.integer(x)
}

args <- commandArgs(trailingOnly = TRUE)
if (length(args) < 2L || length(args) > 3L) {
  stop("usage: serial_runtime_diagnose.R out.rds label [out.csv]", call. = FALSE)
}
out_rds <- args[[1L]]
label <- args[[2L]]
out_csv <- if (length(args) >= 3L) args[[3L]] else ""

small_iters <- parse_int(Sys.getenv("DIAG_SMALL_ITERS"), 5L)
heavy_iters <- parse_int(Sys.getenv("DIAG_HEAVY_ITERS"), 3L)
stopifnot(small_iters >= 1L, heavy_iters >= 1L)

has_mtlapply <- exists("mtlapply", mode = "function")
has_stats <- has_mtlapply && nzchar(Sys.getenv("R_MTL_RUNTIME_STATS"))
has_shared_stats <- has_mtlapply && nzchar(Sys.getenv("R_MTL_SHARED_STATS"))

to_named_num <- function(x, prefix) {
  if (length(x) == 0L) return(setNames(numeric(0), character(0)))
  y <- as.numeric(x)
  names(y) <- paste0(prefix, names(x))
  y
}

capture_stats <- function() {
  rt <- numeric()
  sh <- numeric()
  if (has_stats) rt <- to_named_num(.Internal(mtlruntimestats(FALSE)), "runtime.")
  if (has_shared_stats) sh <- to_named_num(.Internal(mtlsharedenvstats(FALSE)), "shared.")
  c(rt, sh)
}

reset_stats <- function() {
  if (has_stats) invisible(.Internal(mtlruntimestats(TRUE)))
  if (has_shared_stats) invisible(.Internal(mtlsharedenvstats(TRUE)))
}

## Heavier readme-like workload components.
N <- 3000000L
nshards <- 64L
ngroups <- 4096L
feat_loops <- 80L
alloc_m <- 450000L
alloc_k <- 512L
x <- (as.double(seq_len(N) %% 1000L) - 500) / 10
y <- (as.double((seq_len(N) * 17L) %% 1000L) - 500) / 10
w <- (as.double((seq_len(N) * 31L) %% 1000L) + 1) / 1000
grp <- rep_len(seq_len(ngroups), N)
ids <- seq_len(nshards)

workloads <- list(
  list(
    name = "no_alloc_loop",
    iters = small_iters,
    expr = quote({
      s <- 0.0
      for (i in 1:4e7) s <- s + i
      s
    })
  ),
  list(
    name = "lookup_only",
    iters = small_iters,
    expr = quote({
      x <- 0.0
      for (i in 1:2e7) x <- x + sin(i)
      x
    })
  ),
  list(
    name = "alloc_small",
    iters = small_iters,
    expr = quote({
      s <- 0.0
      for (i in 1:6e5) {
        v <- (i / 10) + 1:64
        a <- list(v, v * v, sqrt(v), v + 1)
        s <- s + sum(a[[1L]]) + sum(a[[2L]]) + sum(a[[3L]]) + sum(a[[4L]])
      }
      s
    })
  ),
  list(
    name = "readme_alloc_pressure",
    iters = heavy_iters,
    expr = quote({
      worker <- function(shard_id) {
        is <- seq.int(shard_id, alloc_m, by = nshards)
        s <- 0.0
        for (i in is) {
          v <- (as.double(i) / 10) + seq_len(alloc_k)
          a <- list(v, v * v, sqrt(v), v + 1)
          s <- s + sum(a[[1L]]) + sum(a[[2L]]) + sum(a[[3L]]) + sum(a[[4L]])
        }
        s
      }
      parts <- lapply(ids, worker)
      out <- 0.0
      for (p in parts) out <- out + p
      out
    })
  ),
  list(
    name = "readme_etl_group_mean",
    iters = heavy_iters,
    expr = quote({
      worker <- function(shard_id) {
        idx <- seq.int(shard_id, N, by = nshards)
        z <- x[idx]
        yy <- y[idx]
        ww <- w[idx]
        for (i in seq_len(feat_loops)) {
          z <- log1p(abs(z)) + sin(yy + z) * ww + cos(z - yy)
        }
        g <- grp[idx]
        n <- tabulate(g, ngroups)
        s <- numeric(ngroups)
        for (j in seq_along(z)) s[[g[[j]]]] <- s[[g[[j]]]] + z[[j]]
        list(sum = s, n = n)
      }
      parts <- lapply(ids, worker)
      s <- Reduce(`+`, lapply(parts, `[[`, "sum"))
      n <- Reduce(`+`, lapply(parts, `[[`, "n"))
      s / n
    })
  )
)

rows <- list()
for (wl in workloads) {
  for (i in seq_len(wl$iters)) {
    invisible(gc())
    reset_stats()
    elapsed <- unname(system.time(eval(wl$expr, parent.frame()))[["elapsed"]])
    stats <- capture_stats()
    row <- c(label = label, workload = wl$name, rep = i, elapsed = elapsed, stats)
    rows[[length(rows) + 1L]] <- row
  }
}

all_names <- unique(unlist(lapply(rows, names), use.names = FALSE))
run_df <- do.call(rbind, lapply(rows, function(r) {
  out <- setNames(rep(NA_real_, length(all_names)), all_names)
  out[names(r)] <- r
  out
}))
run_df <- as.data.frame(run_df, stringsAsFactors = FALSE)
run_df$label <- as.character(run_df$label)
run_df$workload <- as.character(run_df$workload)
run_df$rep <- as.integer(run_df$rep)
num_cols <- setdiff(names(run_df), c("label", "workload"))
for (nm in num_cols) run_df[[nm]] <- as.numeric(run_df[[nm]])

summary_df <- do.call(rbind, lapply(split(run_df, run_df$workload), function(d) {
  data.frame(
    label = unique(d$label),
    workload = unique(d$workload),
    reps = nrow(d),
    median_elapsed = median(d$elapsed),
    mean_elapsed = mean(d$elapsed),
    sd_elapsed = sd(d$elapsed),
    min_elapsed = min(d$elapsed),
    max_elapsed = max(d$elapsed),
    stringsAsFactors = FALSE
  )
}))

obj <- list(
  meta = list(
    label = label,
    r_version = R.version.string,
    r_platform = R.version$platform,
    r_home = R.home(),
    has_mtlapply = has_mtlapply,
    has_stats = has_stats,
    has_shared_stats = has_shared_stats,
    small_iters = small_iters,
    heavy_iters = heavy_iters
  ),
  summary = summary_df,
  runs = run_df
)

dir.create(dirname(out_rds), recursive = TRUE, showWarnings = FALSE)
saveRDS(obj, out_rds)
cat("wrote: ", out_rds, "\n", sep = "")

if (nzchar(out_csv)) {
  dir.create(dirname(out_csv), recursive = TRUE, showWarnings = FALSE)
  write.csv(run_df, out_csv, row.names = FALSE)
  cat("wrote: ", out_csv, "\n", sep = "")
}
