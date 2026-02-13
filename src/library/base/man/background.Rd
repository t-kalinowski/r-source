% File src/library/base/man/background.Rd
\name{background}
\alias{background}
\alias{then}
\alias{wait}
\alias{cancel}
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
\code{mtlapply()} and controlled by \code{options(mtlapply.threads = n)}.
For large \code{mtlapply()} inputs, chunking is controlled by
\code{options(mtlapply.chunk_size = n)}.
}
\examples{
old <- getOption("mtlapply.threads")
options(mtlapply.threads = 2L)
on.exit(options(mtlapply.threads = old))

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
