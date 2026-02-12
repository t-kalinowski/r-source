% File src/library/base/man/mtlapply.Rd
% Part of the R package, https://www.R-project.org
% Copyright 2026 R Core Team
% Distributed under GPL 2 or later

\name{mtlapply}
\title{Apply a Function over a List Using Threads (Experimental)}
\alias{mtlapply}
\usage{
mtlapply(X, FUN, \dots)
}
\arguments{
  \item{X}{a vector (atomic or list).  Other objects (including classed
    objects) will be coerced by \code{base::\link{as.list}}.}
  \item{FUN}{the function to be applied to each element of \code{X}.}
  \item{\dots}{optional arguments to \code{FUN}.}
}
\description{
  \code{mtlapply} returns a list of the same length as \code{X}, each
  element of which is the result of applying \code{FUN} to the
  corresponding element of \code{X}, computed using a pool of worker
  threads.
}
\details{
  This is an experimental function intended as a starting point for
  thread-based evaluation in R.

  In this implementation, \code{\dots} are evaluated in the calling thread
  (as in \code{\link[parallel]{mclapply}} on Unix-alikes), and the worker
  threads evaluate \code{FUN} concurrently.

  The worker-pool size is controlled by \code{options(mtlapply.threads = n)}
  and defaults to \code{2L}.

  Large inputs are evaluated in chunks. The chunk size is controlled by
  \code{options(mtlapply.chunk_size = n)} and defaults to \code{50000L}.
  This is a tuning knob and does not change results.

  Workers use the real \code{globalenv()}. Reads from global bindings are
  allowed, but writes to \code{globalenv()} from worker threads are rejected
  with an error.

  The worker function must not attempt to mutate process-global state.
  Superassignment (\code{<<-}) is allowed when the target resolves to an
  existing non-global binding (for example, a closure-captured local). Any
  worker write that resolves to \code{globalenv()} still errors.

  Worker \code{options()} writes are job-local: each worker starts from a
  snapshot of main-thread options, and changes do not propagate back to
  the main thread. Setting \code{options(threads = ...)} or
  \code{options(mtlapply.threads = ...)} from a worker thread is not
  supported and errors.

  Some operations that rely on shared process-wide state may run under a
  global internal lock (for example, \code{tempfile()}), and can
  serialize across workers.

  For now, just-in-time compilation of closures is disabled in worker
  threads.
}
\value{
  A list of the same length as \code{X}.
}
\seealso{
  \code{\link{lapply}}, \code{\link[parallel]{mclapply}}.
}
\examples{
x <- as.list(1:5)
old <- getOption("mtlapply.threads")
options(mtlapply.threads = 2L)
on.exit(options(mtlapply.threads = old))
mtlapply(x, function(z) z * 2)
}
