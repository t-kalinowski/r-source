% File src/library/base/man/background.Rd
\name{background}
\alias{background}
\alias{wait}
\alias{cancel}
\title{Queue Work on the Experimental Thread Pool}
\description{
Queue expression evaluation on the experimental multi-thread worker pool and
wait for the next completed result.
}
\usage{
background(expr, env = parent.frame())
wait(futures, timeout = Inf)
cancel(future)
}
\arguments{
  \item{expr}{An expression to evaluate on a worker thread.}
  \item{env}{Environment used for evaluating \code{expr}.}
  \item{futures}{An \code{mt_future} object or list of \code{mt_future} objects.}
  \item{timeout}{Maximum time to wait in seconds. Use \code{0} for polling and
  \code{Inf} to wait indefinitely.}
  \item{future}{An \code{mt_future} object or list of \code{mt_future} objects.}
}
\value{
\code{background()} returns an \code{mt_future} object with fields
\code{expr}, \code{env}, and \code{value}.\cr
\code{wait()} returns the next completed \code{mt_future} from the input set,
or \code{NULL} on timeout. The returned object has attributes
\code{index}, \code{ok}, and \code{cancelled}.\cr
\code{cancel()} requests cancellation and returns \code{TRUE} for futures that
were pending or running at cancellation time.
}
\details{
This interface is experimental. The worker pool is shared with
\code{mtlapply()} and controlled by \code{options(mtlapply.threads = n)}.
}
\examples{
old <- getOption("mtlapply.threads")
options(mtlapply.threads = 2L)
on.exit(options(mtlapply.threads = old))

f1 <- background({ sum(seq_len(1000L)) })
f2 <- background({ sum(seq_len(2000L)) })

pending <- list(f1, f2)
out <- integer(2)
while (length(pending)) {
  got <- wait(pending)
  out[attr(got, "index")] <- got$value
  pending <- pending[-attr(got, "index")]
}
out
}
