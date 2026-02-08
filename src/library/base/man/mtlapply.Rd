% File src/library/base/man/mtlapply.Rd
% Part of the R package, https://www.R-project.org
% Copyright 2026 R Core Team
% Distributed under GPL 2 or later

\name{mtlapply}
\title{Apply a Function over a List Using Threads (Experimental)}
\alias{mtlapply}
\usage{
mtlapply(X, FUN, \dots, threads = 2L)
}
\arguments{
  \item{X}{a vector (atomic or list).  Other objects (including classed
    objects) will be coerced by \code{base::\link{as.list}}.}
  \item{FUN}{the function to be applied to each element of \code{X}.}
  \item{\dots}{optional arguments to \code{FUN}.}
  \item{threads}{integer giving the number of worker threads.}
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
  threads serialize entry to the interpreter via a global lock.  This means
  \code{mtlapply} is not expected to speed up CPU-bound computations.

  The worker function must not attempt to mutate process-global state.
  In particular, \code{options()} can be used to query options, but
  setting options from worker threads is not supported and results in an
  error.
}
\value{
  A list of the same length as \code{X}.
}
\seealso{
  \code{\link{lapply}}, \code{\link[parallel]{mclapply}}.
}
\examples{
x <- as.list(1:5)
mtlapply(x, function(z) z * 2, threads = 2L)
}
