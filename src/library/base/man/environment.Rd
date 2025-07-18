% File src/library/base/man/environment.Rd
% Part of the R package, https://www.R-project.org
% Copyright 1995-2025 R Core Team
% Distributed under GPL 2 or later

\name{environment}
\alias{environment}
\alias{environment<-}
\alias{.GlobalEnv}
\alias{globalenv}
\alias{emptyenv}
\alias{baseenv}
\alias{is.environment}
\alias{new.env}
\alias{parent.env}
\alias{parent.env<-}
\alias{.BaseNamespaceEnv}
\alias{environmentName}
\alias{env.profile}
\alias{enclosure}

\title{Environment Access}
\description{
  Get, set, test for and create environments.
}
\usage{
environment(fun = NULL)
environment(fun) <- value

is.environment(x)

.GlobalEnv
globalenv()
.BaseNamespaceEnv

emptyenv()
baseenv()

new.env(hash = TRUE, parent = parent.frame(), size = 29L)

parent.env(env)
parent.env(env) <- value

environmentName(env)

env.profile(env)
}
\arguments{
  \item{fun}{a \code{\link{function}}, a \code{\link{formula}}, or
    \code{NULL}, which is the default.}
  \item{value}{an environment to associate with the function.}
  \item{x}{an arbitrary \R object.}
  \item{hash}{a logical, if \code{TRUE} the environment will use a hash table.}
  \item{parent}{an environment to be used as the enclosure of the
    environment created.}
  \item{env}{an environment.}
  \item{size}{an integer specifying the initial size for a hashed
    environment.  An internal default value will be used if
    \code{size} is \code{NA} or zero.  This argument is ignored if
    \code{hash} is \code{FALSE}.}
}
\value{
  If \code{fun} is a function or a formula then \code{environment(fun)}
  returns the environment associated with that function or formula.
  If \code{fun} is \code{NULL} then the current evaluation environment is
  returned.

  The replacement form sets the environment of the function or formula
  \code{fun} to the \code{value} given.
  Note that \code{\link{primitive}} functions \code{fun} have no environment and
  trying to set it to a non-\code{NULL} value is deprecated.

  \code{is.environment(obj)} returns \code{TRUE} if and only if
  \code{obj} is an \code{environment}.

  \code{new.env} returns a new (empty) environment with (by default)
  enclosure the parent frame.

  \code{parent.env} returns the enclosing environment of its argument.

  \code{parent.env<-} sets the enclosing environment of its first
  argument.

  \code{environmentName} returns a character string, that given when
  the environment is printed or \code{""} if it is not a named environment.

  \code{env.profile} returns a list with the following components:
  \code{size} the number of chains that can be stored in the hash table,
  \code{nchains} the number of non-empty chains in the table (as
  reported by \code{HASHPRI}), and \code{counts} an integer vector
  giving the length of each chain (zero for empty chains).  This
  function is intended to assess the performance of hashed environments.
  When \code{env} is a non-hashed environment, \code{NULL} is returned.
}

\details{
  Environments consist of a \emph{frame}, or collection of named
  objects, and a pointer to an \emph{enclosing environment}.  The most
  common example is the frame of variables local to a function call; its
  \emph{enclosure} is the environment where the function was defined
  (unless changed subsequently).  The enclosing environment is
  distinguished from the \emph{parent frame}: the latter (returned by
  \code{\link{parent.frame}}) refers to the environment of the caller of
  a function.  Since confusion is so easy, it is best never to use
  \sQuote{parent} in connection with an environment (despite the
  presence of the function \code{parent.env}).

  When \code{\link{get}} or \code{\link{exists}} search an environment
  with the default \code{inherits = TRUE}, they look for the variable
  in the frame, then in the enclosing frame, and so on.

  The global environment \code{.GlobalEnv}, more often known as the
  user's workspace, is the first item on the search path.  It can also
  be accessed by \code{globalenv()}.  On the search path, each item's
  enclosure is the next item.

  The object \code{.BaseNamespaceEnv} is the namespace environment for
  the base package.  The environment of the base package itself is
  available as \code{baseenv()}.

  If one follows the chain of enclosures found by repeatedly calling
  \code{parent.env} from any environment, eventually one reaches the
  empty environment \code{emptyenv()}, into which nothing may
  be assigned.

  The replacement function \code{parent.env<-} is extremely dangerous as
  it can be used to destructively change environments in ways that
  violate assumptions made by the internal C code.  It may be removed
  in the near future.

  The replacement form of \code{environment}, \code{is.environment},
  \code{baseenv}, \code{emptyenv} and \code{globalenv} are
  \link{primitive} functions.

  System environments, such as the base, global and empty environments,
  have names as do the package and namespace environments and those
  generated by \code{attach()}.  Other environments can be named by
  giving a \code{"name"} attribute, but this needs to be done with care
  as environments have unusual copying semantics.
}
\seealso{
  For the performance implications of hashing or not, see
  \url{https://en.wikipedia.org/wiki/Hash_table}.

  The \code{envir} argument of \code{\link{eval}}, \code{\link{get}},
  and \code{\link{exists}}.

  \code{\link{ls}} may be used to view the objects in an environment,
  and hence \code{\link{ls.str}} may be useful for an overview.

  \code{\link{sys.source}} can be used to populate an environment.
}
\examples{
f <- function() "top level function"

##-- all three give the same:
environment()
environment(f)
.GlobalEnv

ls(envir = environment(stats::approxfun(1:2, 1:2, method = "const")))

is.environment(.GlobalEnv) # TRUE

e1 <- new.env(parent = baseenv())  # this one has enclosure package:base.
e2 <- new.env(parent = e1)
assign("a", 3, envir = e1)
ls(e1)
ls(e2)
exists("a", envir = e2)   # this succeeds by inheritance
exists("a", envir = e2, inherits = FALSE)
exists("+", envir = e2)   # this succeeds by inheritance

eh <- new.env(hash = TRUE, size = NA)
with(env.profile(eh), stopifnot(size == length(counts)))
}
\keyword{data}
\keyword{programming}
