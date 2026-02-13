% File src/library/base/man/background.Rd
\name{background}
\alias{background}
\alias{then}
\alias{wait}
\alias{cancel}
\alias{.mt_capture_env}
\alias{.mt_capture_fn}
\alias{.mt_expr_capture_names}
\alias{.mt_fn_capture_names}
\alias{.mt_notify_drain}
\alias{.mt_notify_fd}
\alias{.mtl_force_namespace}
\title{Queue Work on the Experimental Thread Pool}
\description{
Queue expression evaluation on the experimental multi-thread worker pool and
wait for the next completed result.
}
\usage{
background(expr, env = parent.frame())
then(future, fn, ...)
wait(futures, timeout = Inf)
cancel(future)
}
\arguments{
  \item{expr}{An expression to evaluate on a worker thread.}
  \item{env}{Environment used for evaluating \code{expr}.}
  \item{fn}{Function run after \code{future} resolves. The resolved value is
  passed as the first argument to \code{fn}.}
  \item{\dots}{Additional arguments passed to \code{fn}.}
  \item{futures}{An \code{mt_future} object or list of \code{mt_future} objects.}
  \item{timeout}{Maximum time to wait in seconds. Use \code{0} for polling and
  \code{Inf} to wait indefinitely.}
  \item{future}{An \code{mt_future} object or list of \code{mt_future} objects.}
}
\value{
\code{background()} returns an \code{mt_future} object with fields
\code{expr}, \code{env}, and \code{value}.\cr
\code{then()} returns a new \code{mt_future} chained to \code{future}.\cr
\code{wait()} returns the next completed \code{mt_future} from the input set,
or \code{NULL} on timeout. The returned object has attributes
\code{index}, \code{ok}, and \code{cancelled}.\cr
\code{cancel()} requests cancellation and returns \code{TRUE} for futures that
were pending or running at cancellation time.
}
\details{
This interface is experimental. The worker pool is shared with
\code{mtlapply()} and controlled by \code{options(threads = n)}.
For large \code{mtlapply()} inputs, chunking is controlled by
\code{options(mtlapply.chunk_size = n)}.

For \code{background()}, symbols referenced by \code{expr} are captured from
\code{env} at submission time into a snapshot environment (active bindings are
not supported). This avoids races with subsequent main-thread mutation of the
original frame while workers are running, while avoiding full-frame copies.

Objects with names prefixed by \code{.mt_} and \code{.mtl_} are internal helper
utilities and not part of the public API.
}
\keyword{internal}
\examples{
old <- getOption("threads")
options(threads = 2L)
on.exit(options(threads = old))

f1 <- background({ sum(seq_len(1000L)) })
f2 <- background({ sum(seq_len(2000L)) })
f3 <- then(f1, function(x) x + 1L)

pending <- list(f1, f2, f3)
out <- integer(3)
while (length(pending)) {
  got <- wait(pending)
  out[attr(got, "index")] <- got$value
  pending <- pending[-attr(got, "index")]
}
out
}
